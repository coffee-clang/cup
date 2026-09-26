/*
 * Plans package updates from one refreshed catalog snapshot. Installed state is checked before
 * network access, then each planned family is revalidated by package_install under exclusive
 * authority before it can mutate anything.
 */

#include "commands.h"

#include "catalog_refresh.h"
#include "command_context.h"
#include "package_artifact.h"
#include "package_install.h"
#include "package_selector.h"
#include "registry.h"
#include "self_update.h"
#include "state.h"
#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    UPDATE_ACTION_NO_STABLE,
    UPDATE_ACTION_AHEAD,
    UPDATE_ACTION_EQUAL,
    UPDATE_ACTION_NEWER
} UpdateAction;

typedef struct {
    PackageIdentity reference;
    PackageIdentity previous_default;
    int has_previous_default;
    int reference_advertised;
    UpdateAction action;
    char stable_version[MAX_IDENTIFIER_LEN];
    PackageArtifactSpec artifact_spec;
} UpdatePlanItem;

typedef struct {
    UpdatePlanItem *items;
    size_t count;
    size_t capacity;
} UpdatePlan;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    int component_request;
    int has_selector;
} UpdateFilter;

static void update_plan_free(UpdatePlan *plan) {
    if (plan != NULL) {
        free(plan->items);
        memset(plan, 0, sizeof(*plan));
    }
}

static CupError update_filter_init(UpdateFilter *filter, const char *name) {
    CupError err;

    if (filter == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(filter, 0, sizeof(*filter));
    if (text_is_empty(name)) {
        return CUP_OK;
    }
    filter->has_selector = 1;
    if (registry_is_component(name)) {
        filter->component_request = 1;
        return text_copy(filter->component, sizeof(filter->component), name);
    }
    err = registry_find_tool_component(name, filter->component, sizeof(filter->component));
    if (err == CUP_OK) {
        err = text_copy(filter->tool, sizeof(filter->tool), name);
    }
    return err;
}

static int update_filter_matches(const UpdateFilter *filter,
                                 const PackageIdentity *identity,
                                 const char *host) {
    if (filter == NULL || identity == NULL || text_is_empty(host) ||
        strcmp(identity->host_platform, host) != 0) {
        return 0;
    }
    if (!filter->has_selector) {
        return 1;
    }
    if (strcmp(identity->component, filter->component) != 0) {
        return 0;
    }
    return filter->component_request || strcmp(identity->tool, filter->tool) == 0;
}

static int update_plan_find(const UpdatePlan *plan,
                            const char *component,
                            const char *tool,
                            const char *target) {
    size_t i;

    for (i = 0; i < plan->count; ++i) {
        const PackageIdentity *identity = &plan->items[i].reference;
        if (strcmp(identity->component, component) == 0 && strcmp(identity->tool, tool) == 0 &&
            strcmp(identity->target_platform, target) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static CupError update_plan_reserve(UpdatePlan *plan) {
    UpdatePlanItem *items;
    size_t capacity;

    if (plan->count < plan->capacity) {
        return CUP_OK;
    }
    capacity = plan->capacity == 0 ? 8u : plan->capacity * 2u;
    if (capacity < plan->capacity || capacity > SIZE_MAX / sizeof(*items)) {
        return CUP_ERR_STATE_FULL;
    }
    items = realloc(plan->items, capacity * sizeof(*items));
    if (items == NULL) {
        return CUP_ERR_TEMPORARY;
    }
    plan->items = items;
    plan->capacity = capacity;
    return CUP_OK;
}

/* Local-only precheck prevents an empty update from touching the network. */
static CupError update_precheck(const UpdateFilter *filter, int *has_match) {
    CommandContext context;
    CupError err;
    size_t i;

    if (filter == NULL || has_match == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_match = 0;
    err = command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        command_context_end(&context);
        return err;
    }
    err = command_context_load_state(&context);
    if (err == CUP_OK) {
        for (i = 0; i < context.state.installed_count; ++i) {
            if (update_filter_matches(filter, &context.state.installed[i], context.host_platform)) {
                *has_match = 1;
                break;
            }
        }
    }
    command_context_end(&context);
    return err;
}

static CupError update_plan_add(UpdatePlan *plan,
                                const CommandContext *context,
                                const PackageIdentity *installed) {
    UpdatePlanItem candidate;
    CupError err;
    int compared;

    if (update_plan_find(
            plan, installed->component, installed->tool, installed->target_platform) >= 0) {
        return CUP_OK;
    }
    memset(&candidate, 0, sizeof(candidate));
    {
        PackageScope scope;
        int reference_is_default = 0;

        err = package_scope_init(&scope,
                                 installed->component,
                                 context->host_platform,
                                 installed->target_platform);
        if (err == CUP_OK) {
            err = state_get_tool_reference(&context->state,
                                           &scope,
                                           installed->tool,
                                           &candidate.reference,
                                           &reference_is_default);
        }
        if (err == CUP_OK && reference_is_default) {
            candidate.previous_default = candidate.reference;
            candidate.has_previous_default = 1;
        }
    }
    if (err != CUP_OK) {
        return err;
    }
    err = package_catalog_has_version(&context->catalog,
                                      candidate.reference.component,
                                      candidate.reference.tool,
                                      candidate.reference.host_platform,
                                      candidate.reference.target_platform,
                                      candidate.reference.version,
                                      &candidate.reference_advertised);
    if (err != CUP_OK) {
        return err;
    }
    err = package_catalog_resolve_stable(&context->catalog,
                                         candidate.stable_version,
                                         sizeof(candidate.stable_version),
                                         candidate.reference.component,
                                         candidate.reference.tool,
                                         candidate.reference.host_platform,
                                         candidate.reference.target_platform);
    if (err == CUP_ERR_NOT_AVAILABLE) {
        candidate.action = UPDATE_ACTION_NO_STABLE;
    } else if (err != CUP_OK) {
        return err;
    } else {
        err = package_release_compare(candidate.stable_version, candidate.reference.version, &compared);
        if (err != CUP_OK) {
            return err;
        }
        if (compared < 0) {
            candidate.action = UPDATE_ACTION_AHEAD;
        } else {
            candidate.action = compared == 0 ? UPDATE_ACTION_EQUAL : UPDATE_ACTION_NEWER;
            err = package_artifact_spec_resolve_stable(&candidate.artifact_spec,
                                                       &context->catalog,
                                                       candidate.reference.component,
                                                       candidate.reference.tool,
                                                       candidate.reference.host_platform,
                                                       candidate.reference.target_platform);
            if (err != CUP_OK) {
                return err;
            }
        }
    }

    err = update_plan_reserve(plan);
    if (err != CUP_OK) {
        return err;
    }
    plan->items[plan->count++] = candidate;
    return CUP_OK;
}

/* Build a complete logical plan from the one catalog snapshot obtained after refresh. */
static CupError update_plan_build(const UpdateFilter *filter, UpdatePlan *plan) {
    CommandContext context;
    CupError err;
    size_t i;

    if (filter == NULL || plan == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(plan, 0, sizeof(*plan));
    err = command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        command_context_end(&context);
        return err;
    }
    err = command_context_load_state(&context);
    if (err == CUP_OK) {
        err = command_context_load_catalog(&context);
    }
    if (err == CUP_OK) {
        for (i = 0; i < context.state.installed_count; ++i) {
            const PackageIdentity *installed = &context.state.installed[i];
            if (!update_filter_matches(filter, installed, context.host_platform)) {
                continue;
            }
            err = update_plan_add(plan, &context, installed);
            if (err != CUP_OK) {
                break;
            }
        }
    }
    command_context_end(&context);
    return err;
}

static CupError update_catalog(void) {
    CommandContext context;
    CupError err;
    int updated = 0;

    err = command_context_begin_initialize(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }
#if !CUP_VERSION_OFFICIAL
    err = package_catalog_seed_runtime();
#endif
    if (err == CUP_OK) {
        err = command_context_load_catalog(&context);
    }
    command_context_end(&context);
    if (err != CUP_OK) {
        return err;
    }

    err = catalog_refresh_existing(&updated, CATALOG_REFRESH_REPORT_ERRORS);
    if (err == CUP_OK) {
        printf(updated ? "Catalog updated.\n" : "Catalog is already current.\n");
    }
    return err;
}

static void print_skip(const UpdatePlanItem *item) {
    if (item->action == UPDATE_ACTION_NO_STABLE) {
        if (!item->reference_advertised) {
            fprintf(stderr,
                    "Warning: %s:%s@%s for target '%s' is installed locally but is not "
                    "advertised by the current catalog, and no usable stable is available; "
                    "keeping the local package unchanged.\n",
                    item->reference.component,
                    item->reference.tool,
                    item->reference.version,
                    item->reference.target_platform);
        } else {
            fprintf(stderr,
                    "Warning: no usable stable is available for %s:%s on target '%s'; "
                    "keeping %s unchanged.\n",
                    item->reference.component,
                    item->reference.tool,
                    item->reference.target_platform,
                    item->reference.version);
        }
    } else if (item->action == UPDATE_ACTION_AHEAD) {
        fprintf(stderr,
                "Warning: installed %s:%s@%s for target '%s' is newer than catalog "
                "stable %s; keeping the installed version.\n",
                item->reference.component,
                item->reference.tool,
                item->reference.version,
                item->reference.target_platform,
                item->stable_version);
    }
}

CupError command_update(const char *selector) {
    UpdateFilter filter;
    UpdatePlan plan = {0};
    CupError err;
    const char *name = selector;
    const char *label;
    size_t i;
    size_t installed_count = 0;
    size_t moved_default_count = 0;
    size_t skipped_count = 0;
    int has_match = 0;
    int catalog_updated = 0;

    if (!text_is_empty(name) && strcmp(name, "cup") == 0) {
        return self_update_start();
    }
    if (!text_is_empty(name) && strcmp(name, "catalog") == 0) {
        return update_catalog();
    }

    err = update_filter_init(&filter, name);
    if (err != CUP_OK) {
        return err;
    }
    label = text_is_empty(name) ? "all installed tools" : name;

    err = update_precheck(&filter, &has_match);
    if (err != CUP_OK) {
        return err;
    }
    if (!has_match) {
        if (!filter.has_selector) {
            printf("No installed tools to update.\n");
            return CUP_OK;
        }
        fprintf(stderr, "Error: no installed packages match '%s'.\n", name);
        return CUP_ERR_NOT_AVAILABLE;
    }

    /* Package update requires one committed catalog refresh before any package planning. */
    err = catalog_refresh_existing(&catalog_updated, CATALOG_REFRESH_REPORT_ERRORS);
    if (err != CUP_OK) {
        return err;
    }

    err = update_plan_build(&filter, &plan);
    if (err != CUP_OK) {
        update_plan_free(&plan);
        return err;
    }
    if (plan.count == 0) {
        update_plan_free(&plan);
        return filter.has_selector ? CUP_ERR_NOT_AVAILABLE : CUP_OK;
    }

    for (i = 0; i < plan.count; ++i) {
        UpdatePlanItem *item = &plan.items[i];
        int installed = 0;
        int default_moved = 0;

        if (item->action == UPDATE_ACTION_NO_STABLE || item->action == UPDATE_ACTION_AHEAD) {
            print_skip(item);
            skipped_count++;
            continue;
        }

        if (item->action == UPDATE_ACTION_EQUAL) {
            printf("==> Checking %s@%s for target '%s'...\n",
                   item->reference.tool,
                   item->reference.version,
                   item->reference.target_platform);
        } else {
            printf("==> Updating %s for target '%s' (%s -> %s)...\n",
                   item->reference.tool,
                   item->reference.target_platform,
                   item->reference.version,
                   item->artifact_spec.identity.version);
        }
        err = package_install_update_artifact(&item->artifact_spec,
                                              &item->reference,
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
                    "Update for '%s' stopped at package %zu of %zu: "
                    "%zu installed, %zu default(s) updated, %zu skipped. "
                    "Previous releases were kept.\n",
                    label,
                    i + 1,
                    plan.count,
                    installed_count,
                    moved_default_count,
                    skipped_count);
            update_plan_free(&plan);
            return err;
        }
        installed_count += (size_t)installed;
        moved_default_count += (size_t)default_moved;
    }

    printf("Update complete for '%s': %zu installed, %zu default(s) updated, "
           "%zu skipped. Previous releases were kept.\n",
           label,
           installed_count,
           moved_default_count,
           skipped_count);
    update_plan_free(&plan);
    return CUP_OK;
}
