#include "commands.h"

#include "command_context.h"
#include "entry.h"
#include "entrypoints.h"
#include "info.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

/* Package info.txt output. */
static void print_info_field(const PackageInfo *info, const char *key,
    const char *label) {
    const char *value = info_get(info, key);

    if (value != NULL) {
        printf("  %-18s %s\n", label, value);
    }
}

static void print_info_group(const PackageInfo *info, const char *title,
    const char *prefix) {
    const PackageInfoField *field;
    size_t cursor = 0;
    size_t prefix_length = strlen(prefix);
    int printed = 0;

    while ((field = info_next(info, prefix, &cursor)) != NULL) {
        if (!printed) {
            printf("\n%s:\n", title);
            printed = 1;
        }

        printf("  %-18s %s\n", field->key + prefix_length, field->value);
    }
}

static void print_package_info(const PackageInfo *info) {
    printf("Package:\n");
    print_info_field(info, "package.component", "component");
    print_info_field(info, "package.tool", "tool");
    print_info_field(info, "package.version", "version");
    print_info_field(info, "platform.host", "host");
    print_info_field(info, "platform.target", "target");
    print_info_group(info, "Entries", "entry.");
    print_info_group(info, "Features", "features.");
    print_info_group(info, "Contents", "contents.");
    print_info_group(info, "Build/config", "config.");
}

/* Installed-package listing. */
CupError command_list(const char *component, const char *target_override) {
    CommandContext context = {0};
    CupError err;
    size_t i;
    int printed = 0;

    if (component != NULL) {
        err = registry_validate_component(component);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    command_context_try_manifest(&context);

    for (i = 0; i < context.state.installed_count; ++i) {
        const StateEntry *state_entry = &context.state.installed[i];
        PackageIdentity package;
        const char *default_entry;
        int is_on_disk = 0;
        int is_stable = 0;
        int has_annotation = 0;

        if (strcmp(state_entry->host_platform, context.host_platform) != 0 ||
            strcmp(state_entry->target_platform, context.target_platform) != 0 ||
            (component != NULL &&
                strcmp(state_entry->component, component) != 0)) {
            continue;
        }

        if (!printed) {
            if (component == NULL) {
                printf("Installed packages for host '%s', target '%s':\n",
                    context.host_platform, context.target_platform);
            } else {
                printf("Installed %s packages for host '%s', target '%s':\n",
                    component, context.host_platform, context.target_platform);
            }
            printed = 1;
        }

        printf("- %s:%s", state_entry->component, state_entry->entry);

        if (package_identity_from_entry(&package, state_entry->component,
            state_entry->host_platform, state_entry->target_platform,
            state_entry->entry) != CUP_OK ||
            package_path_exists(&package, &is_on_disk) != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        if (!is_on_disk) {
            printf(" (missing on disk)\n");
            continue;
        }

        {
            char install_path[MAX_PATH_LEN];

            if (layout_build_install_path(install_path,
                sizeof(install_path), &package) != CUP_OK ||
                package_validate(install_path, &package) != CUP_OK) {
                printf(" (invalid on disk)\n");
                continue;
            }
        }

        default_entry = state_get_default(&context.state, state_entry->component,
            state_entry->host_platform, state_entry->target_platform);

        if (context.has_manifest) {
            manifest_is_stable(&context.manifest, state_entry->component,
                package.tool, package.host_platform, package.target_platform,
                package.version, &is_stable);
        }

        if ((default_entry != NULL &&
            strcmp(default_entry, state_entry->entry) == 0) || is_stable) {
            printf(" (");

            if (default_entry != NULL &&
                strcmp(default_entry, state_entry->entry) == 0) {
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
        if (component == NULL) {
            printf("No packages installed for host '%s', target '%s'.\n",
                context.host_platform, context.target_platform);
        } else {
            printf("No %s packages installed for host '%s', target '%s'.\n",
                component, context.host_platform, context.target_platform);
        }
    }

    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Default selection. */
CupError command_default(const char *component, const char *entry,
    const char *target_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    CupState candidate;
    EntryPointPlan entrypoints;
    CupError err;

    entrypoint_plan_init(&entrypoints);

    if (component == NULL || entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = entry_request_parse(component, entry, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin(&context, target_override,
        SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_no_transaction();
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    if (entry_is_stable(request.release)) {
        err = command_context_load_manifest(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = entry_request_resolve(context.has_manifest
            ? &context.manifest : NULL,
        component, context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform,
        request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_valid_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    candidate = context.state;
    err = state_set_default(&candidate, component,
        context.host_platform, context.target_platform,
        request.resolved_entry);
    if (err != CUP_OK) {
        goto done;
    }

    err = entrypoint_plan_build(&entrypoints, &candidate);
    if (err != CUP_OK) {
        goto done;
    }

    context.state = candidate;
    err = state_save(&context.state);
    if (err != CUP_OK) {
        goto done;
    }

    err = entrypoint_plan_apply(&entrypoints);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: the default was saved, but its entry points could not "
            "be rebuilt. Run 'cup repair'.\n");
        err = CUP_ERR_COMMIT;
        goto done;
    }

    printf("Default %s for host '%s', target '%s' set to ",
        component, context.host_platform, context.target_platform);
    entry_request_print(stdout, &request);
    printf(".\n");

done:
    entrypoint_plan_free(&entrypoints);
    command_context_end(&context);
    return err;
}

/* Current defaults. */
static CupError print_current_entry(const CommandContext *context,
    const StateEntry *state_entry) {
    PackageIdentity package;
    EntryPointPlan entrypoints;
    CupError err;
    int entrypoints_match;
    int is_stable = 0;
    size_t i;

    entrypoint_plan_init(&entrypoints);
    err = package_identity_from_entry(&package, state_entry->component,
        state_entry->host_platform, state_entry->target_platform,
        state_entry->entry);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = command_require_valid_installed(context, &package);
    if (err != CUP_OK) {
        return err;
    }

    if (context->has_manifest) {
        manifest_is_stable(&context->manifest, package.component,
            package.tool, package.host_platform, package.target_platform,
            package.version, &is_stable);
    }

    err = entrypoint_plan_build_default(&entrypoints, state_entry);
    if (err != CUP_OK) {
        entrypoint_plan_free(&entrypoints);
        return err;
    }

    err = entrypoint_plan_expected_matches(&entrypoints, &entrypoints_match);
    if (err != CUP_OK || !entrypoints_match) {
        entrypoint_plan_free(&entrypoints);
        return err == CUP_OK ? CUP_ERR_INCONSISTENT_STATE : err;
    }

    printf("- %s [%s]: %s%s\n", state_entry->component,
        state_entry->target_platform, state_entry->entry,
        is_stable ? " (stable)" : "");
    printf("  commands: ");
    if (entrypoints.count == 0) {
        printf("(none)");
    } else {
        for (i = 0; i < entrypoints.count; ++i) {
            printf("%s%s", i == 0 ? "" : ", ", entrypoints.items[i].name);
        }
    }
    printf("\n  status: active\n");

    entrypoint_plan_free(&entrypoints);
    return CUP_OK;
}

CupError command_current(const char *component,
    const char *target_override) {
    CommandContext context = {0};
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

    err = command_context_begin(&context, target_override,
        SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    command_context_try_manifest(&context);

    for (i = 0; i < context.state.default_count; ++i) {
        const StateEntry *state_entry = &context.state.defaults[i];

        if (strcmp(state_entry->host_platform, context.host_platform) != 0 ||
            (target_override != NULL &&
                strcmp(state_entry->target_platform,
                    context.target_platform) != 0) ||
            (component != NULL &&
                strcmp(state_entry->component, component) != 0)) {
            continue;
        }

        if (!printed) {
            if (component != NULL && target_override != NULL) {
                printf("Current default for component '%s', host '%s', "
                    "target '%s':\n", component, context.host_platform,
                    context.target_platform);
            } else if (component != NULL) {
                printf("Current defaults for component '%s' on host '%s':\n",
                    component, context.host_platform);
            } else if (target_override != NULL) {
                printf("Current defaults for host '%s', target '%s':\n",
                    context.host_platform, context.target_platform);
            } else {
                printf("Current defaults for host '%s':\n",
                    context.host_platform);
            }
            printed = 1;
        }

        err = print_current_entry(&context, state_entry);
        if (err != CUP_OK) {
            printf("- %s [%s]: %s\n  status: invalid\n",
                state_entry->component, state_entry->target_platform,
                state_entry->entry);
            invalid = 1;
        }
    }

    if (!printed) {
        if (component != NULL && target_override != NULL) {
            printf("No current default for component '%s' on host '%s', "
                "target '%s'.\n", component, context.host_platform,
                context.target_platform);
        } else if (component != NULL) {
            printf("No current defaults for component '%s' on host '%s'.\n",
                component, context.host_platform);
        } else if (target_override != NULL) {
            printf("No current defaults for host '%s', target '%s'.\n",
                context.host_platform, context.target_platform);
        } else {
            printf("No current defaults for host '%s'.\n",
                context.host_platform);
        }
    }

    err = invalid ? CUP_ERR_INCONSISTENT_STATE : CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Manifest catalog output. */
static int catalog_matches(const ManifestPackage *package,
    const char *component, const char *host, const char *target) {
    return strcmp(package->host_platform, host) == 0 &&
        (component == NULL || strcmp(package->component, component) == 0) &&
        (target == NULL || strcmp(package->target_platform, target) == 0);
}

static int component_seen_before(const Manifest *manifest, size_t index,
    const char *component, const char *host, const char *target) {
    size_t i;

    for (i = 0; i < index; ++i) {
        const ManifestPackage *package = &manifest->packages[i];

        if (catalog_matches(package, NULL, host, target) &&
            strcmp(package->component, component) == 0) {
            return 1;
        }
    }
    return 0;
}

static int tool_seen_before(const Manifest *manifest, size_t index,
    const char *component, const char *tool, const char *host,
    const char *target) {
    size_t i;

    for (i = 0; i < index; ++i) {
        const ManifestPackage *package = &manifest->packages[i];

        if (catalog_matches(package, component, host, target) &&
            strcmp(package->tool, tool) == 0) {
            return 1;
        }
    }
    return 0;
}

static int print_catalog_tools(const Manifest *manifest,
    const char *component, const char *host, const char *target,
    const char *tool_indent, const char *scope_indent) {
    size_t i;
    int printed = 0;

    for (i = 0; i < manifest->count; ++i) {
        const ManifestPackage *package = &manifest->packages[i];
        size_t j;

        if (!catalog_matches(package, component, host, target) ||
            tool_seen_before(manifest, i, package->component,
                package->tool, host, target)) {
            continue;
        }

        printf("%s%s\n", tool_indent, package->tool);
        printed = 1;

        for (j = 0; j < manifest->count; ++j) {
            const ManifestPackage *scope = &manifest->packages[j];

            if (!catalog_matches(scope, component, host, target) ||
                strcmp(scope->tool, package->tool) != 0) {
                continue;
            }

            printf("%s%s: stable %s; versions %s\n",
                scope_indent, scope->target_platform,
                scope->stable_version, scope->available_versions);
        }
    }

    return printed;
}

static void print_manifest_catalog(const Manifest *manifest,
    const char *component, const char *host, const char *target) {
    size_t i;
    int printed = 0;

    if (component != NULL) {
        if (target == NULL) {
            printf("Available tools for component '%s' on host '%s':\n\n",
                component, host);
        } else {
            printf("Available tools for component '%s', host '%s', "
                "target '%s':\n\n", component, host, target);
        }

        printed = print_catalog_tools(manifest, component, host, target,
            "", "  ");
    } else {
        if (target == NULL) {
            printf("Available packages for host '%s':\n", host);
        } else {
            printf("Available packages for host '%s', target '%s':\n",
                host, target);
        }

        for (i = 0; i < manifest->count; ++i) {
            const ManifestPackage *package = &manifest->packages[i];

            if (!catalog_matches(package, NULL, host, target) ||
                component_seen_before(manifest, i, package->component,
                    host, target)) {
                continue;
            }

            printf("\n%s:\n", package->component);
            if (print_catalog_tools(manifest, package->component,
                    host, target, "  ", "    ")) {
                printed = 1;
            }
        }
    }

    if (!printed) {
        if (component == NULL) {
            printf("No packages are available for the selected host and target.\n");
        } else {
            printf("No tools are available for component '%s' on the selected "
                "host and target.\n", component);
        }
    }
}

static CupError show_catalog(const char *component,
    const char *target_override) {
    CommandContext context = {0};
    CupError err;
    const char *target = NULL;

    if (component != NULL) {
        err = registry_validate_component(component);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_manifest(&context);
    if (err != CUP_OK) {
        goto done;
    }

    if (target_override != NULL) {
        target = context.target_platform;
    }
    print_manifest_catalog(&context.manifest, component,
        context.host_platform, target);
    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

/* Catalog and installed package metadata. */
CupError command_info(const char *component, const char *entry,
    const char *target_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    PackageInfo info;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char info_path[MAX_PATH_LEN];

    if (entry == NULL) {
        return show_catalog(component, target_override);
    }
    if (component == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    info_init(&info);

    err = entry_request_parse(component, entry, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    if (entry_is_stable(request.release)) {
        err = command_context_load_manifest(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = entry_request_resolve(context.has_manifest
            ? &context.manifest : NULL,
        component, context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform,
        request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_valid_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), &package);
    if (err != CUP_OK ||
        path_join(info_path, sizeof(info_path), install_path,
            CUP_INFO_FILENAME) != CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
        goto done;
    }

    err = info_load(&info, info_path);
    if (err != CUP_OK) {
        goto done;
    }

    printf("Package information for %s ", component);
    entry_request_print(stdout, &request);
    printf(" on host '%s', target '%s':\n\n",
        context.host_platform, context.target_platform);
    print_package_info(&info);
    err = CUP_OK;

done:
    info_free(&info);
    command_context_end(&context);
    return err;
}
