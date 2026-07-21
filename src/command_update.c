/*
 * Builds tool or component update plans from installed scopes, then revalidates and updates each
 * scope independently under an exclusive lock.
 */

#include "commands.h"

#include "command_context.h"
#include "package_selector.h"
#include "package_install.h"
#include "registry.h"
#include "state.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Stable-update plan built from one consistent state snapshot. */
typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
    char previous_active[MAX_SELECTOR_LEN];
} UpdatePlanItem;

typedef struct {
    UpdatePlanItem items[MAX_INSTALLED];
    size_t count;
} UpdatePlan;

static int plan_find(const UpdatePlan *plan,
                     const char *component,
                     const char *tool,
                     const char *target) {
    size_t i;

    for (i = 0; i < plan->count; ++i) {
        if (strcmp(plan->items[i].component, component) == 0 &&
            strcmp(plan->items[i].tool, tool) == 0 && strcmp(plan->items[i].target, target) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static CupError plan_add(UpdatePlan *plan,
                         const CommandContext *context,
                         const PackageIdentity *installed) {
    UpdatePlanItem candidate = {0};
    const PackageIdentity *active_identity;
    PackageScope scope;
    CupError err;

    if (package_identity_validate(installed) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (plan_find(plan, installed->component, installed->tool, installed->target_platform) >= 0) {
        return CUP_OK;
    }
    if (plan->count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    err = text_copy(candidate.component, sizeof(candidate.component), installed->component);
    if (err == CUP_OK) {
        err = text_copy(candidate.tool, sizeof(candidate.tool), installed->tool);
    }
    if (err == CUP_OK) {
        err = text_copy(candidate.target, sizeof(candidate.target), installed->target_platform);
    }
    if (err != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    err = package_scope_init(&scope, candidate.component, context->host_platform, candidate.target);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    active_identity = state_get_active(&context->state, &scope);
    if (active_identity != NULL && package_identity_validate(active_identity) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (active_identity != NULL && strcmp(active_identity->tool, candidate.tool) == 0 &&
        package_identity_format_selector(active_identity,
                                         candidate.previous_active,
                                         sizeof(candidate.previous_active)) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    plan->items[plan->count++] = candidate;
    return CUP_OK;
}

/* Initial read-only scan of installed scopes. */
static CupError build_update_plan(const char *name, UpdatePlan *plan) {
    CommandContext context = {0};
    CupError err;
    char requested_component[MAX_IDENTIFIER_LEN] = "";
    char requested_tool[MAX_IDENTIFIER_LEN] = "";
    int component_request;
    size_t i;

    memset(plan, 0, sizeof(*plan));
    component_request = !text_is_empty(name) && registry_is_component(name);
    if (text_is_empty(name)) {
        err = CUP_OK;
    } else if (component_request) {
        err = text_copy(requested_component, sizeof(requested_component), name);
    } else {
        err = registry_find_tool_component(name, requested_component, sizeof(requested_component));
        if (err == CUP_OK) {
            err = text_copy(requested_tool, sizeof(requested_tool), name);
        }
    }
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }
    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    for (i = 0; i < context.state.installed_count; ++i) {
        const PackageIdentity *installed = &context.state.installed[i];

        if (strcmp(installed->host_platform, context.host_platform) != 0 ||
            (!text_is_empty(name) && strcmp(installed->component, requested_component) != 0)) {
            continue;
        }

        if (package_identity_validate(installed) != CUP_OK) {
            err = CUP_ERR_INCONSISTENT_STATE;
            goto done;
        }
        if (!text_is_empty(name) && !component_request &&
            strcmp(installed->tool, requested_tool) != 0) {
            continue;
        }

        err = plan_add(plan, &context, installed);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Per-scope revalidation and update execution. */
CupError command_update(const char *selector) {
    UpdatePlan plan;
    CupError err;
    char name[MAX_IDENTIFIER_LEN] = "";
    const char *label;
    size_t i;
    size_t installed_count = 0;
    size_t active_count = 0;
    size_t skipped_count = 0;

    if (!text_is_empty(selector) && text_copy_lower_ascii(name, sizeof(name), selector) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (strcmp(name, "cup") == 0) {
        return command_update_cup();
    }

    label = text_is_empty(name) ? "all installed tools" : name;
    err = build_update_plan(text_is_empty(name) ? NULL : name, &plan);
    if (err != CUP_OK) {
        return err;
    }
    if (plan.count == 0) {
        if (text_is_empty(name)) {
            printf("No installed tool scopes to update.\n");
        } else {
            printf("No installed scopes match '%s'.\n", name);
        }
        return CUP_OK;
    }

    for (i = 0; i < plan.count; ++i) {
        UpdatePlanItem *item = &plan.items[i];
        int installed = 0;
        int active_moved = 0;

        printf(
            "==> Updating %s:%s for target '%s'...\n", item->component, item->tool, item->target);
        err = package_install_update_scope(item->component,
                                           item->tool,
                                           item->target,
                                           item->previous_active,
                                           &installed,
                                           &active_moved);
        if (err == CUP_ERR_NOT_INSTALLED) {
            skipped_count++;
            continue;
        }
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Update for '%s' stopped while processing scope %zu of %zu: "
                    "%zu stable package(s) installed, %zu default(s) moved, "
                    "%zu scope(s) skipped. Previous releases were retained.\n",
                    label,
                    i + 1,
                    plan.count,
                    installed_count,
                    active_count,
                    skipped_count);
            if (err == CUP_ERR_COMMIT) {
                fprintf(stderr,
                        "The current scope may also have committed; run 'cup repair' "
                        "before continuing.\n");
            }
            return err;
        }
        installed_count += (size_t)installed;
        active_count += (size_t)active_moved;
    }

    printf("Update completed for '%s': %zu stable package(s) installed, "
           "%zu default(s) moved, %zu scope(s) skipped; previous releases "
           "were retained.\n",
           label,
           installed_count,
           active_count,
           skipped_count);
    return CUP_OK;
}
