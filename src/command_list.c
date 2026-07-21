/*
 * Lists installed packages with deterministic scope and active annotations.
 */

#include "commands.h"

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

/* Installed-package listing. */
CupError command_list(const char *component, const char *target_override) {
    CommandContext context = {0};
    PackageIdentity entries[MAX_INSTALLED];
    size_t entry_count = 0;
    CupError err;
    size_t i;
    int printed = 0;
    int degraded = 0;

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

    for (i = 0; i < context.state.installed_count; ++i) {
        const PackageIdentity *candidate = &context.state.installed[i];

        if (strcmp(candidate->host_platform, context.host_platform) == 0 &&
            (target_override == NULL ||
             strcmp(candidate->target_platform, context.target_platform) == 0) &&
            (component == NULL || strcmp(candidate->component, component) == 0)) {
            entries[entry_count++] = *candidate;
        }
    }
    qsort(entries, entry_count, sizeof(entries[0]), compare_identity);

    for (i = 0; i < entry_count; ++i) {
        const PackageIdentity *package = &entries[i];
        const PackageIdentity *active_identity;
        PackageScope scope;
        char selector[MAX_SELECTOR_LEN];
        int is_on_disk = 0;
        int is_stable = 0;
        int has_annotation = 0;

        if (!printed) {
            if (component == NULL && target_override == NULL) {
                printf("Installed packages for host '%s':\n", context.host_platform);
            } else if (component == NULL) {
                printf("Installed packages for host '%s', target '%s':\n",
                       context.host_platform,
                       context.target_platform);
            } else if (target_override == NULL) {
                printf("Installed %s packages for host '%s':\n", component, context.host_platform);
            } else {
                printf("Installed %s packages for host '%s', target '%s':\n",
                       component,
                       context.host_platform,
                       context.target_platform);
            }
            printed = 1;
        }

        if (package_identity_format_selector(package, selector, sizeof(selector)) != CUP_OK) {
            printf("- %s:(invalid state record)\n", package->component);
            degraded = 1;
            continue;
        }

        printf("- %s:%s", package->component, selector);
        if (target_override == NULL) {
            printf(" [target %s]", package->target_platform);
        }

        err = package_path_exists(package, &is_on_disk);
        if (err != CUP_OK) {
            printf(" (could not inspect package path)\n");
            degraded = 1;
            continue;
        }
        if (!is_on_disk) {
            printf(" (missing on disk)\n");
            degraded = 1;
            continue;
        }

        {
            char install_path[MAX_PATH_LEN];

            err = layout_build_install_path(install_path, sizeof(install_path), package);
            if (err != CUP_OK) {
                printf(" (could not construct package path)\n");
                degraded = 1;
                continue;
            }

            err = package_validate(install_path, package);
            if (err == CUP_ERR_VALIDATION) {
                printf(" (invalid on disk)\n");
                degraded = 1;
                continue;
            }
            if (err != CUP_OK) {
                printf(" (could not inspect package)\n");
                degraded = 1;
                continue;
            }
        }

        err = package_identity_get_scope(package, &scope);
        if (err != CUP_OK) {
            printf(" (invalid state record)\n");
            degraded = 1;
            continue;
        }
        active_identity = state_get_active(&context.state, &scope);

        if (context.has_catalog) {
            package_catalog_is_stable(&context.catalog,
                                      package->component,
                                      package->tool,
                                      package->host_platform,
                                      package->target_platform,
                                      package->version,
                                      &is_stable);
        }

        if ((active_identity != NULL && package_identity_equals(active_identity, package)) ||
            is_stable) {
            printf(" (");

            if (active_identity != NULL && package_identity_equals(active_identity, package)) {
                printf("default");
                has_annotation = 1;
            }

            if (is_stable) {
                printf("%sstable", has_annotation ? ", " : "");
            }

            printf(")");
        }

        printf("\n");
    }

    if (!printed) {
        if (component == NULL && target_override == NULL) {
            printf("No packages installed for host '%s'.\n", context.host_platform);
        } else if (component == NULL) {
            printf("No packages installed for host '%s', target '%s'.\n",
                   context.host_platform,
                   context.target_platform);
        } else if (target_override == NULL) {
            printf("No %s packages installed for host '%s'.\n", component, context.host_platform);
        } else {
            printf("No %s packages installed for host '%s', target '%s'.\n",
                   component,
                   context.host_platform,
                   context.target_platform);
        }
    }

    err = degraded ? CUP_ERR_INCONSISTENT_STATE : CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Default selection. */
