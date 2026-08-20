/*
 * Shows effective defaults and their managed commands for one or all components.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "package.h"
#include "package_catalog.h"
#include "state.h"
#include "wrappers.h"

#include <stdio.h>
#include <string.h>

/* Snapshot matching defaults before rendering so state insertion order cannot affect output. */
static size_t collect_info_entries(const CommandContext *context,
                                   const char *component,
                                   const char *target_override,
                                   PackageIdentity *entries) {
    size_t count = 0;
    size_t i;

    for (i = 0; i < context->state.default_count; ++i) {
        const PackageIdentity *candidate = &context->state.defaults[i];

        if (package_identity_matches(candidate,
                                     context->host_platform,
                                     target_override == NULL ? NULL : context->target_platform,
                                     component)) {
            entries[count++] = *candidate;
        }
    }
    package_identity_sort(entries, count);
    return count;
}

static void print_info_heading(const CommandContext *context,
                               const char *component,
                               const char *target_override) {
    if (component != NULL && target_override != NULL) {
        printf("Default for component '%s', host '%s', target '%s':\n",
               component,
               context->host_platform,
               context->target_platform);
    } else if (component != NULL) {
        printf("Defaults for component '%s' on host '%s':\n",
               component,
               context->host_platform);
    } else if (target_override != NULL) {
        printf("Defaults for host '%s', target '%s':\n",
               context->host_platform,
               context->target_platform);
    } else {
        printf("Defaults for host '%s':\n", context->host_platform);
    }
}

static void print_empty_info(const CommandContext *context,
                             const char *component,
                             const char *target_override) {
    if (component != NULL && target_override != NULL) {
        printf("No default for component '%s' on host '%s', target '%s'.\n",
               component,
               context->host_platform,
               context->target_platform);
    } else if (component != NULL) {
        printf("No defaults for component '%s' on host '%s'.\n",
               component,
               context->host_platform);
    } else if (target_override != NULL) {
        printf("No defaults for host '%s', target '%s'.\n",
               context->host_platform,
               context->target_platform);
    } else {
        printf("No defaults for host '%s'.\n", context->host_platform);
    }
}

/* Validate one default identity and render the wrappers derived from that exact package. */
static CupError print_info_entry(const CommandContext *context,
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

    err = wrapper_plan_build_default(&wrappers, identity);
    if (err != CUP_OK) {
        wrapper_plan_free(&wrappers);
        return err;
    }

    err = wrapper_plan_entries_match(&wrappers, &wrappers_match);
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
    printf("\n  status: default\n");

    wrapper_plan_free(&wrappers);
    return CUP_OK;
}

static int print_info_entries(const CommandContext *context,
                              const PackageIdentity *entries,
                              size_t entry_count) {
    int invalid = 0;
    size_t i;

    for (i = 0; i < entry_count; ++i) {
        const PackageIdentity *entry = &entries[i];
        CupError err = print_info_entry(context, entry);

        if (err != CUP_OK) {
            char selector[MAX_SELECTOR_LEN] = "(invalid identity)";

            package_identity_format_selector(entry, selector, sizeof(selector));
            printf("- %s [%s]: %s\n  status: invalid\n",
                   entry->component,
                   entry->target_platform,
                   selector);
            invalid = 1;
        }
    }
    return invalid;
}

CupError command_info(const char *component, const char *target_override) {
    CommandContext context = {0};
    PackageIdentity entries[MAX_INSTALLED];
    size_t entry_count;
    CupError err;
    CupError catalog_err = CUP_OK;
    int invalid;

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
    catalog_err = command_context_load_catalog(&context);

    entry_count = collect_info_entries(&context, component, target_override, entries);
    if (entry_count == 0) {
        print_empty_info(&context, component, target_override);
        err = catalog_err;
        goto done;
    }

    print_info_heading(&context, component, target_override);
    invalid = print_info_entries(&context, entries, entry_count);
    err = invalid ? CUP_ERR_INCONSISTENT_STATE : catalog_err;

done:
    command_context_end(&context);
    return err;
}
