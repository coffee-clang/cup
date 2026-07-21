/*
 * Presents package catalog availability without mutating local state.
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

static int compare_catalog_entry(const void *left_value, const void *right_value) {
    const PackageCatalogEntry *left = left_value;
    const PackageCatalogEntry *right = right_value;
    int result;

    result = strcmp(left->component, right->component);
    if (result == 0) {
        result = strcmp(left->tool, right->tool);
    }
    if (result == 0) {
        result = strcmp(left->target_platform, right->target_platform);
    }
    return result;
}

static int catalog_matches(const PackageCatalogEntry *package,
                           const char *component,
                           const char *host,
                           const char *target) {
    return strcmp(package->host_platform, host) == 0 &&
           (component == NULL || strcmp(package->component, component) == 0) &&
           (target == NULL || strcmp(package->target_platform, target) == 0);
}

static int component_seen_before(const PackageCatalog *catalog,
                                 size_t index,
                                 const char *component,
                                 const char *host,
                                 const char *target) {
    size_t i;

    for (i = 0; i < index; ++i) {
        const PackageCatalogEntry *package = &catalog->packages[i];

        if (catalog_matches(package, NULL, host, target) &&
            strcmp(package->component, component) == 0) {
            return 1;
        }
    }
    return 0;
}

static int tool_seen_before(const PackageCatalog *catalog,
                            size_t index,
                            const char *component,
                            const char *tool,
                            const char *host,
                            const char *target) {
    size_t i;

    for (i = 0; i < index; ++i) {
        const PackageCatalogEntry *package = &catalog->packages[i];

        if (catalog_matches(package, component, host, target) && strcmp(package->tool, tool) == 0) {
            return 1;
        }
    }
    return 0;
}

static int print_catalog_tools(const PackageCatalog *catalog,
                               const char *component,
                               const char *host,
                               const char *target,
                               const char *tool_indent,
                               const char *scope_indent) {
    size_t i;
    int printed = 0;

    for (i = 0; i < catalog->count; ++i) {
        const PackageCatalogEntry *package = &catalog->packages[i];
        size_t j;

        if (!catalog_matches(package, component, host, target) ||
            tool_seen_before(catalog, i, package->component, package->tool, host, target)) {
            continue;
        }

        printf("%s%s\n", tool_indent, package->tool);
        printed = 1;

        for (j = 0; j < catalog->count; ++j) {
            const PackageCatalogEntry *scope = &catalog->packages[j];

            if (!catalog_matches(scope, component, host, target) ||
                strcmp(scope->tool, package->tool) != 0) {
                continue;
            }

            printf("%s%s: stable %s; versions %s\n",
                   scope_indent,
                   scope->target_platform,
                   scope->stable_version,
                   scope->available_versions);
        }
    }

    return printed;
}

static void print_package_catalog(const PackageCatalog *catalog,
                                  const char *component,
                                  const char *host,
                                  const char *target) {
    size_t i;
    int printed = 0;

    if (component != NULL) {
        if (target == NULL) {
            printf("Available tools for component '%s' on host '%s':\n\n", component, host);
        } else {
            printf("Available tools for component '%s', host '%s', "
                   "target '%s':\n\n",
                   component,
                   host,
                   target);
        }

        printed = print_catalog_tools(catalog, component, host, target, "", "  ");
    } else {
        if (target == NULL) {
            printf("Available packages for host '%s':\n", host);
        } else {
            printf("Available packages for host '%s', target '%s':\n", host, target);
        }

        for (i = 0; i < catalog->count; ++i) {
            const PackageCatalogEntry *package = &catalog->packages[i];

            if (!catalog_matches(package, NULL, host, target) ||
                component_seen_before(catalog, i, package->component, host, target)) {
                continue;
            }

            printf("\n%s:\n", package->component);
            if (print_catalog_tools(catalog, package->component, host, target, "  ", "    ")) {
                printed = 1;
            }
        }
    }

    if (!printed) {
        if (component == NULL) {
            printf("No packages are available for the selected host and target.\n");
        } else {
            printf("No tools are available for component '%s' on the selected "
                   "host and target.\n",
                   component);
        }
    }
}

CupError command_search(const char *component, const char *target_override) {
    CommandContext context = {0};
    CupError err;
    const char *target = NULL;

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

    err = command_context_load_catalog(&context);
    if (err != CUP_OK) {
        goto done;
    }

    qsort(context.catalog.packages,
          context.catalog.count,
          sizeof(context.catalog.packages[0]),
          compare_catalog_entry);
    if (target_override != NULL) {
        target = context.target_platform;
    }
    print_package_catalog(&context.catalog, component, context.host_platform, target);
    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Installed package metadata. */
