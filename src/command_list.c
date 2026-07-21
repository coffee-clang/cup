/*
 * Lists installed packages with deterministic scope and active annotations.
 */

#include "commands.h"

#include "command_context.h"
#include "layout.h"
#include "package.h"
#include "package_catalog.h"
#include "package_metadata.h"
#include "package_selector.h"
#include "registry.h"
#include "state.h"
#include "wrappers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stable ordering keeps output independent of state-file insertion history. */
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

static int package_matches_scope(const PackageIdentity *package,
                                 const CommandContext *context,
                                 const char *component,
                                 const char *target_override) {
    return strcmp(package->host_platform, context->host_platform) == 0 &&
           (target_override == NULL ||
            strcmp(package->target_platform, context->target_platform) == 0) &&
           (component == NULL || strcmp(package->component, component) == 0);
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
        printf("- %s:(invalid state record)\n", package->component);
        *degraded = 1;
        return 0;
    }

    printf("- %s:%s", package->component, selector);
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

    err = package_validate(install_path, package);
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
    const PackageIdentity *active_identity;
    PackageScope scope;
    int is_default;
    int is_stable = 0;

    if (package_identity_get_scope(package, &scope) != CUP_OK) {
        printf(" (invalid state record)\n");
        return 0;
    }
    active_identity = state_get_active(&context->state, &scope);
    is_default = active_identity != NULL && package_identity_equals(active_identity, package);

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
    CommandContext context = {0};
    PackageIdentity entries[MAX_INSTALLED];
    size_t entry_count = 0;
    CupError err;
    size_t i;
    int degraded = 0;

    if (component != NULL) {
        err = registry_validate_component(component);
        if (err != CUP_OK) {
            return err;
        }
    }

    /* Open the read-only host view without initializing an unused CUP root. */
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

    /* Snapshot and sort matching records before any filesystem inspection or output. */
    for (i = 0; i < context.state.installed_count; ++i) {
        const PackageIdentity *candidate = &context.state.installed[i];

        if (package_matches_scope(candidate, &context, component, target_override)) {
            entries[entry_count++] = *candidate;
        }
    }
    qsort(entries, entry_count, sizeof(entries[0]), compare_identity);

    if (entry_count == 0) {
        print_empty_list(&context, component, target_override);
        err = CUP_OK;
        goto done;
    }

    print_list_heading(&context, component, target_override);
    for (i = 0; i < entry_count; ++i) {
        if (!print_package_health(&entries[i], target_override, &degraded)) {
            continue;
        }
        if (!print_package_annotations(&context, &entries[i])) {
            degraded = 1;
        }
    }
    err = degraded ? CUP_ERR_INCONSISTENT_STATE : CUP_OK;

done:
    command_context_end(&context);
    return err;
}
