/*
 * Shows effective active package information for one or all components.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "package_selector.h"
#include "wrappers.h"
#include "package_metadata.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_identity(const void *left_value, const void *right_value) {
    const PackageIdentity *left = left_value;
    const PackageIdentity *right = right_value;
    int result;

    result = strcmp(left->target_platform, right->target_platform);
    if (result == 0) {
        result = strcmp(left->component, right->component);
    }
    if (result == 0) {
        result = strcmp(left->tool, right->tool);
    }
    if (result == 0) {
        result = strcmp(left->version, right->version);
    }
    return result;
}

static CupError print_current_entry(const CommandContext *context,
                                    const PackageIdentity *identity) {
    WrapperPlan wrappers;
    CupError err;
    char selector[MAX_SELECTOR_LEN];
    int wrappers_match;
    int is_stable = 0;
    size_t i;

    wrapper_plan_init(&wrappers);
    if (package_identity_format_selector(identity, selector, sizeof(selector)) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = installed_package_require_valid(&context->state, identity);
    if (err != CUP_OK) {
        return err;
    }

    if (context->has_catalog) {
        package_catalog_is_stable(&context->catalog,
                                  identity->component,
                                  identity->tool,
                                  identity->host_platform,
                                  identity->target_platform,
                                  identity->version,
                                  &is_stable);
    }

    err = wrapper_plan_build_active(&wrappers, identity);
    if (err != CUP_OK) {
        wrapper_plan_free(&wrappers);
        return err;
    }

    err = wrapper_plan_expected_matches(&wrappers, &wrappers_match);
    if (err != CUP_OK || !wrappers_match) {
        wrapper_plan_free(&wrappers);
        return err == CUP_OK ? CUP_ERR_INCONSISTENT_STATE : err;
    }

    printf("- %s [%s]: %s%s\n",
           identity->component,
           identity->target_platform,
           selector,
           is_stable ? " (stable)" : "");
    printf("  commands: ");
    if (wrappers.count == 0) {
        printf("(none)");
    } else {
        for (i = 0; i < wrappers.count; ++i) {
            printf("%s%s", i == 0 ? "" : ", ", wrappers.items[i].name);
        }
    }
    printf("\n  status: active\n");

    wrapper_plan_free(&wrappers);
    return CUP_OK;
}

CupError command_info(const char *component, const char *target_override) {
    CommandContext context = {0};
    PackageIdentity entries[MAX_INSTALLED];
    size_t entry_count = 0;
    CupError err;
    size_t i;
    int printed = 0;
    int invalid = 0;

    if (component != NULL) {
        err = registry_validate_component(component);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = command_context_begin_read_only(&context, target_override);
    if (err != CUP_OK) {
        goto done;
    }

    if (!context.runtime_available) {
        memset(&context.state, 0, sizeof(context.state));
    } else {
        err = command_context_load_state(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    command_context_try_catalog(&context);

    for (i = 0; i < context.state.active_count; ++i) {
        const PackageIdentity *candidate = &context.state.active[i];

        if (strcmp(candidate->host_platform, context.host_platform) == 0 &&
            (target_override == NULL ||
             strcmp(candidate->target_platform, context.target_platform) == 0) &&
            (component == NULL || strcmp(candidate->component, component) == 0)) {
            entries[entry_count++] = *candidate;
        }
    }
    qsort(entries, entry_count, sizeof(entries[0]), compare_identity);

    for (i = 0; i < entry_count; ++i) {
        const PackageIdentity *state_entry = &entries[i];

        if (!printed) {
            if (component != NULL && target_override != NULL) {
                printf("Current default for component '%s', host '%s', "
                       "target '%s':\n",
                       component,
                       context.host_platform,
                       context.target_platform);
            } else if (component != NULL) {
                printf("Current defaults for component '%s' on host '%s':\n",
                       component,
                       context.host_platform);
            } else if (target_override != NULL) {
                printf("Current defaults for host '%s', target '%s':\n",
                       context.host_platform,
                       context.target_platform);
            } else {
                printf("Current defaults for host '%s':\n", context.host_platform);
            }
            printed = 1;
        }

        err = print_current_entry(&context, state_entry);
        if (err != CUP_OK) {
            char selector[MAX_SELECTOR_LEN] = "(invalid identity)";

            package_identity_format_selector(state_entry, selector, sizeof(selector));
            printf("- %s [%s]: %s\n  status: invalid\n",
                   state_entry->component,
                   state_entry->target_platform,
                   selector);
            invalid = 1;
        }
    }

    if (!printed) {
        if (component != NULL && target_override != NULL) {
            printf("No current default for component '%s' on host '%s', "
                   "target '%s'.\n",
                   component,
                   context.host_platform,
                   context.target_platform);
        } else if (component != NULL) {
            printf("No current defaults for component '%s' on host '%s'.\n",
                   component,
                   context.host_platform);
        } else if (target_override != NULL) {
            printf("No current defaults for host '%s', target '%s'.\n",
                   context.host_platform,
                   context.target_platform);
        } else {
            printf("No current defaults for host '%s'.\n", context.host_platform);
        }
    }

    err = invalid ? CUP_ERR_INCONSISTENT_STATE : CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* PackageCatalog catalog output. */
