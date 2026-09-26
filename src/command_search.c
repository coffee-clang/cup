/*
 * Presents package catalog availability without mutating package/state authority.
 * Existing runtimes may refresh the live catalog through its dedicated CAS lifecycle.
 */

#include "commands.h"

#include "command_context.h"
#include "catalog_refresh.h"
#include "package_catalog.h"
#include "package_selector.h"

#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Catalog presentation is deterministic even when catalog.cfg changes record order. */
static int compare_catalog_entry(const void *left_value, const void *right_value) {
    const PackageCatalogEntry *left = left_value;
    const PackageCatalogEntry *right = right_value;
    int result;

    result = strcmp(left->component, right->component);
    if (result == 0) {
        result = strcmp(left->tool, right->tool);
    }
    if (result == 0) {
        result = strcmp(left->host_platform, right->host_platform);
    }
    if (result == 0) {
        result = strcmp(left->target_platform, right->target_platform);
    }
    if (result == 0) {
        {
            int version_result = 0;
            result = package_release_compare(left->version, right->version, &version_result) == CUP_OK
                         ? -version_result
                         : -strcmp(left->version, right->version);
        }
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

static int same_tool_scope(const PackageCatalogEntry *left, const PackageCatalogEntry *right) {
    return strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0;
}

static int same_target_scope(const PackageCatalogEntry *left, const PackageCatalogEntry *right) {
    return same_tool_scope(left, right) &&
           strcmp(left->target_platform, right->target_platform) == 0;
}

static size_t print_target_scope(const PackageCatalog *catalog,
                                 size_t start,
                                 const char *scope_indent) {
    const PackageCatalogEntry *first = &catalog->packages[start];
    const char *stable = NULL;
    size_t end = start;
    size_t i;

    while (end < catalog->count && same_target_scope(first, &catalog->packages[end])) {
        if (catalog->packages[end].stable) {
            stable = catalog->packages[end].version;
        }
        end++;
    }

    printf("%s%s: ", scope_indent, first->target_platform);
    for (i = start; i < end; ++i) {
        const PackageCatalogEntry *entry = &catalog->packages[i];
        printf("%s%s", i == start ? "" : ", ", entry->version);
        if (stable != NULL && strcmp(entry->version, stable) == 0) {
            printf(" (stable)");
        }
        if (entry->revision_reason[0] != '\0') {
            printf(" [%s]", entry->revision_reason);
        }
    }
    printf("\n");
    return end;
}

static int print_catalog_tools(const PackageCatalog *catalog,
                               const char *component,
                               const char *host,
                               const char *target,
                               const char *tool_indent,
                               const char *scope_indent) {
    size_t i = 0;
    int printed = 0;

    while (i < catalog->count) {
        const PackageCatalogEntry *package = &catalog->packages[i];
        size_t tool_end;
        size_t j;

        if (!catalog_matches(package, component, host, target)) {
            i++;
            continue;
        }

        printf("%s%s\n", tool_indent, package->tool);
        printed = 1;
        tool_end = i;
        while (tool_end < catalog->count && same_tool_scope(package, &catalog->packages[tool_end])) {
            tool_end++;
        }
        j = i;
        while (j < tool_end) {
            if (target == NULL || strcmp(catalog->packages[j].target_platform, target) == 0) {
                j = print_target_scope(catalog, j, scope_indent);
            } else {
                j++;
            }
        }
        i = tool_end;
    }
    return printed;
}

/* Render one component or the complete component/tool hierarchy. */
static void print_package_catalog(const PackageCatalog *catalog,
                                  const char *component,
                                  const char *host,
                                  const char *target) {
    size_t i = 0;
    int printed = 0;

    if (component != NULL) {
        if (target == NULL) {
            printf("Available tools for component '%s' on host '%s':\n\n", component, host);
        } else {
            printf("Available tools for component '%s', host '%s', target '%s':\n\n",
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

        while (i < catalog->count) {
            const PackageCatalogEntry *package = &catalog->packages[i];
            size_t component_end = i + 1;

            while (component_end < catalog->count &&
                   strcmp(package->component, catalog->packages[component_end].component) == 0) {
                component_end++;
            }
            if (catalog_matches(package, NULL, host, target)) {
                printf("\n%s:\n", package->component);
                if (print_catalog_tools(
                        catalog, package->component, host, target, "  ", "    ")) {
                    printed = 1;
                }
            } else {
                size_t j;
                for (j = i + 1; j < component_end; ++j) {
                    if (catalog_matches(&catalog->packages[j], NULL, host, target)) {
                        printf("\n%s:\n", package->component);
                        if (print_catalog_tools(
                                catalog, package->component, host, target, "  ", "    ")) {
                            printed = 1;
                        }
                        break;
                    }
                }
            }
            i = component_end;
        }
    }

    if (!printed) {
        if (component == NULL) {
            printf("No packages are available for the selected host and target.\n");
        } else {
            printf("No tools are available for component '%s' on the selected host and target.\n",
                   component);
        }
    }
}

/* Search never initializes runtime state. Existing runtimes may refresh the live catalog through
 * the catalog refresh lifecycle before one frozen presentation snapshot is printed. */
CupError command_search(const char *component, const char *target_override) {
    CommandContext context;
    CupError err;
    const char *target = NULL;

    err = command_context_begin_read_only(&context, target_override);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_catalog(&context);
    if (err != CUP_OK) {
        goto done;
    }

    /* Search is the discovery exception: an existing runtime refreshes best-effort, but a
     * source-only checkout never creates/persists a runtime merely to search. */
    if (context.runtime_available) {
        CupError refresh_err;
        int updated = 0;

        command_context_end(&context);
        memset(&context, 0, sizeof(context));
        ui_phase("Refreshing catalog...");
        refresh_err = catalog_refresh_existing(&updated, CATALOG_REFRESH_QUIET);
        if (refresh_err != CUP_OK) {
            fprintf(stderr,
                    "Warning: catalog refresh failed; showing the local catalog.\n");
        }
        err = command_context_begin_read_only(&context, target_override);
        if (err != CUP_OK) {
            goto done;
        }
        err = command_context_load_catalog(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    {
        PackageCatalog visible = {0};
        PackageCatalogEntry *entries = NULL;
        size_t visible_count = 0;
        size_t i;

        if (context.catalog.count != 0) {
            entries = malloc(context.catalog.count * sizeof(*entries));
            if (entries == NULL) {
                err = CUP_ERR_TEMPORARY;
                goto done;
            }
        }
        for (i = 0; i < context.catalog.count; ++i) {
            if (context.catalog.packages[i].operational) {
                entries[visible_count++] = context.catalog.packages[i];
            }
        }
        visible.packages = entries;
        visible.count = visible_count;
        if (visible_count > 1) {
            qsort(visible.packages,
                  visible.count,
                  sizeof(visible.packages[0]),
                  compare_catalog_entry);
        }
        if (target_override != NULL) {
            target = context.target_platform;
        }
        print_package_catalog(&visible, component, context.host_platform, target);
        free(entries);
    }
    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}
