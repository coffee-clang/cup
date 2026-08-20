/*
 * Builds tool or component update plans from installed scopes, then revalidates and updates each
 * scope independently under an exclusive lock.
 */

#include "commands.h"

#include "command_context.h"
#include "cup_update.h"
#include "package_selector.h"
#include "package_install.h"
#include "registry.h"
#include "state.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Stable-update plan built from one consistent state snapshot. */
typedef struct {
    PackageIdentity previous_default;
    int has_previous_default;
    PackageArtifactSpec artifact_spec;
} UpdatePlanItem;

typedef struct {
    UpdatePlanItem items[MAX_INSTALLED];
    size_t count;
} UpdatePlan;

static int update_plan_find(const UpdatePlan *plan,
                            const char *component,
                            const char *tool,
                            const char *target) {
    size_t i;

    for (i = 0; i < plan->count; ++i) {
        const PackageIdentity *identity = &plan->items[i].artifact_spec.identity;

        if (strcmp(identity->component, component) == 0 && strcmp(identity->tool, tool) == 0 &&
            strcmp(identity->target_platform, target) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static CupError update_plan_add(UpdatePlan *plan,
                                const CommandContext *context,
                                const PackageIdentity *installed) {
    UpdatePlanItem candidate = {0};
    const PackageIdentity *default_identity;
    PackageScope scope;
    CupError err;

    if (package_identity_validate(installed, stderr) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (update_plan_find(
            plan, installed->component, installed->tool, installed->target_platform) >= 0) {
        return CUP_OK;
    }
    if (plan->count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    err = package_scope_init(
        &scope, installed->component, context->host_platform, installed->target_platform);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    default_identity = state_get_default(&context->state, &scope);
    if (default_identity != NULL && package_identity_validate(default_identity, stderr) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (default_identity != NULL && strcmp(default_identity->tool, installed->tool) == 0) {
        candidate.previous_default = *default_identity;
        candidate.has_previous_default = 1;
    }

    err = package_artifact_spec_resolve_stable(&candidate.artifact_spec,
                                               &context->catalog,
                                               installed->component,
                                               installed->tool,
                                               context->host_platform,
                                               installed->target_platform);
    if (err != CUP_OK) {
        return err;
    }

    plan->items[plan->count++] = candidate;
    return CUP_OK;
}

/* Initial read-only scan of installed scopes. */
static CupError update_plan_build(const char *name, UpdatePlan *plan) {
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
    if (err == CUP_OK) {
        err = command_context_load_catalog(&context);
    }
    if (err != CUP_OK) {
        goto done;
    }

    for (i = 0; i < context.state.installed_count; ++i) {
        const PackageIdentity *installed = &context.state.installed[i];

        if (strcmp(installed->host_platform, context.host_platform) != 0 ||
            (!text_is_empty(name) && strcmp(installed->component, requested_component) != 0)) {
            continue;
        }

        if (!text_is_empty(name) && !component_request &&
            strcmp(installed->tool, requested_tool) != 0) {
            continue;
        }

        err = update_plan_add(plan, &context, installed);
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
    const char *name = selector;
    const char *label;
    size_t i;
    size_t installed_count = 0;
    size_t moved_default_count = 0;
    size_t skipped_count = 0;

    if (!text_is_empty(name) && strcmp(name, "cup") == 0) {
        return cup_update_start();
    }

    label = text_is_empty(name) ? "all installed tools" : name;
    err = update_plan_build(text_is_empty(name) ? NULL : name, &plan);
    if (err != CUP_OK) {
        return err;
    }
    if (plan.count == 0) {
        if (text_is_empty(name)) {
            printf("No installed tools to update.\n");
        } else {
            printf("No installed packages match '%s'.\n", name);
        }
        return CUP_OK;
    }

    for (i = 0; i < plan.count; ++i) {
        UpdatePlanItem *item = &plan.items[i];
        const PackageIdentity *identity = &item->artifact_spec.identity;
        int installed = 0;
        int default_moved = 0;

        printf(
            "==> Updating %s:%s for target '%s'...\n",
            identity->component,
            identity->tool,
            identity->target_platform);
        err = package_install_update_artifact(&item->artifact_spec,
                                              item->has_previous_default
                                                  ? &item->previous_default
                                                  : NULL,
                                              &installed,
                                              &default_moved);
        if (err == CUP_ERR_NOT_INSTALLED) {
            skipped_count++;
            continue;
        }
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Update for '%s' stopped while processing package %zu of %zu: "
                    "%zu stable package(s) installed, %zu default(s) moved, "
                    "%zu package(s) skipped. Previous releases were retained.\n",
                    label,
                    i + 1,
                    plan.count,
                    installed_count,
                    moved_default_count,
                    skipped_count);
            return err;
        }
        installed_count += (size_t)installed;
        moved_default_count += (size_t)default_moved;
    }

    printf("Update completed for '%s': %zu stable package(s) installed, "
           "%zu default(s) moved, %zu package(s) skipped; previous releases "
           "were retained.\n",
           label,
           installed_count,
           moved_default_count,
           skipped_count);
    return CUP_OK;
}
