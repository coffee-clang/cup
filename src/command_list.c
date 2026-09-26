/*
 * Lists installed packages with deterministic scope and default annotations.
 */

#include "commands.h"

#include "command_context.h"
#include "layout.h"
#include "package.h"
#include "package_catalog.h"
#include "package_selector.h"
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_installed_for_display(const void *left_value, const void *right_value) {
    const PackageIdentity *left = left_value;
    const PackageIdentity *right = right_value;
    int result;

    result = strcmp(left->host_platform, right->host_platform);
    if (result == 0) result = strcmp(left->target_platform, right->target_platform);
    if (result == 0) result = strcmp(left->component, right->component);
    if (result == 0) result = strcmp(left->tool, right->tool);
    if (result == 0) {
        int version_result = 0;
        result = package_release_compare(left->version, right->version, &version_result) == CUP_OK
                     ? -version_result
                     : -strcmp(left->version, right->version);
    }
    return result;
}

static void print_list_heading(const CommandContext *context,
                               const char *component,
                               const char *target_override) {
    if (component == NULL && target_override == NULL) {
        printf("Installed packages for host '%s':\n", context->host_platform);
    } else if (component == NULL) {
        printf("Installed packages for host '%s', target '%s':\n",
               context->host_platform,
               context->target_platform);
    } else if (target_override == NULL) {
        printf("Installed %s packages for host '%s':\n", component, context->host_platform);
    } else {
        printf("Installed %s packages for host '%s', target '%s':\n",
               component,
               context->host_platform,
               context->target_platform);
    }
}

static void print_empty_list(const CommandContext *context,
                             const char *component,
                             const char *target_override) {
    if (component == NULL && target_override == NULL) {
        printf("No packages installed for host '%s'.\n", context->host_platform);
    } else if (component == NULL) {
        printf("No packages installed for host '%s', target '%s'.\n",
               context->host_platform,
               context->target_platform);
    } else if (target_override == NULL) {
        printf("No %s packages installed for host '%s'.\n", component, context->host_platform);
    } else {
        printf("No %s packages installed for host '%s', target '%s'.\n",
               component,
               context->host_platform,
               context->target_platform);
    }
}

/* Print the identity and reject damaged state or package records before annotations are derived. */
static int print_package_health(const PackageIdentity *package,
                                const char *target_override,
                                int *degraded) {
    CupError err;
    char selector[MAX_SELECTOR_LEN];
    char install_path[MAX_PATH_LEN];
    int is_on_disk;

    if (package_identity_format_selector(package, selector, sizeof(selector)) != CUP_OK) {
        printf("- %s: (invalid state record)\n", package->component);
        *degraded = 1;
        return 0;
    }

    printf("- %s: %s", package->component, selector);
    if (target_override == NULL) {
        printf(" [target %s]", package->target_platform);
    }

    err = package_path_exists(package, &is_on_disk);
    if (err != CUP_OK) {
        printf(" (could not inspect package path)\n");
        *degraded = 1;
        return 0;
    }
    if (!is_on_disk) {
        printf(" (missing on disk)\n");
        *degraded = 1;
        return 0;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), package);
    if (err != CUP_OK) {
        printf(" (could not construct package path)\n");
        *degraded = 1;
        return 0;
    }

    err = package_validate(install_path, package, stderr);
    if (err == CUP_ERR_VALIDATION) {
        printf(" (invalid on disk)\n");
        *degraded = 1;
        return 0;
    }
    if (err != CUP_OK) {
        printf(" (could not inspect package)\n");
        *degraded = 1;
        return 0;
    }
    return 1;
}

static int print_package_annotations(const CommandContext *context,
                                     const PackageIdentity *package) {
    const PackageIdentity *default_identity;
    PackageScope scope;
    int is_default;
    int is_stable = 0;

    if (package_identity_get_scope(package, &scope) != CUP_OK) {
        printf(" (invalid state record)\n");
        return 0;
    }
    default_identity = state_get_default(&context->state, &scope);
    is_default = default_identity != NULL && package_identity_equals(default_identity, package);

    if (context->has_catalog) {
        (void)package_catalog_is_stable(&context->catalog,
                                        package->component,
                                        package->tool,
                                        package->host_platform,
                                        package->target_platform,
                                        package->version,
                                        &is_stable);
    }

    if (is_default || is_stable) {
        printf(" (%s%s%s)",
               is_default ? "default" : "",
               is_default && is_stable ? ", " : "",
               is_stable ? "stable" : "");
    }
    printf("\n");
    return 1;
}

/* Validate each selected state entry against disk while continuing to report later entries. */
CupError command_list(const char *component, const char *target_override) {
    CommandContext context;
    size_t entry_count = 0;
    CupError err;
    CupError catalog_err = CUP_OK;
    size_t i;
    int degraded = 0;

    /* Open the read-only host view without initializing an unused cup root. */
    err = command_context_begin_read_only(&context, target_override);
    if (err != CUP_OK) {
        goto done;
    }

    if (context.runtime_available) {
        err = command_context_load_state(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }
    catalog_err = command_context_load_catalog(&context);
    if (catalog_err != CUP_OK) {
        fprintf(stderr,
                "Warning: package catalog unavailable; showing installed packages without stable markers.\n");
    }

    /* Sort the private state snapshot directly; no persistent state is modified. */
    if (context.state.installed_count > 1) {
        qsort(context.state.installed,
              context.state.installed_count,
              sizeof(context.state.installed[0]),
              compare_installed_for_display);
    }
    for (i = 0; i < context.state.installed_count; ++i) {
        if (package_identity_matches(&context.state.installed[i],
                                     context.host_platform,
                                     target_override == NULL ? NULL : context.target_platform,
                                     component)) {
            entry_count++;
        }
    }

    if (entry_count == 0) {
        print_empty_list(&context, component, target_override);
        err = CUP_OK;
        goto done;
    }

    print_list_heading(&context, component, target_override);
    for (i = 0; i < context.state.installed_count; ++i) {
        PackageIdentity *entry = &context.state.installed[i];

        if (!package_identity_matches(entry,
                                      context.host_platform,
                                      target_override == NULL ? NULL : context.target_platform,
                                      component)) {
            continue;
        }
        if (!print_package_health(entry, target_override, &degraded)) {
            continue;
        }
        if (!print_package_annotations(&context, entry)) {
            degraded = 1;
        }
    }
    err = degraded ? CUP_ERR_INCONSISTENT_STATE : CUP_OK;

done:
    command_context_end(&context);
    return err;
}
