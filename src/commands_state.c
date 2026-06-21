#include "commands.h"

#include "command_context.h"
#include "entry.h"
#include "info.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

// INFO OUTPUT
static void print_info_field(const PackageInfo *info, const char *key, const char *label) {
    const char *value;

    value = info_get(info, key);
    if (value != NULL) {
        printf("  %-18s %s\n", label, value);
    }
}

static void print_info_group(const PackageInfo *info, const char *title, const char *prefix) {
    const PackageInfoField *field;
    size_t cursor = 0;
    size_t prefix_length;
    int printed = 0;

    prefix_length = strlen(prefix);

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

// LIST COMMAND
CupError command_list(const char *target_override) {
    CommandContext context = {0};
    CupError err;
    size_t i;
    int printed = 0;

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
            strcmp(state_entry->target_platform, context.target_platform) != 0) {
            continue;
        }

        if (!printed) {
            printf("Installed components for host '%s', target '%s':\n",
                context.host_platform, context.target_platform);
            printed = 1;
        }

        printf("- %s:%s", state_entry->component, state_entry->entry);

        if (package_identity_from_entry(&package, state_entry->component,
            state_entry->host_platform, state_entry->target_platform,
            state_entry->entry) != CUP_OK ||
            package_directory_exists(&package, &is_on_disk) != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        if (!is_on_disk) {
            printf(" (missing on disk)\n");
            continue;
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
        printf("No components installed for host '%s', target '%s'.\n",
            context.host_platform, context.target_platform);
    }

    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

// DEFAULT COMMAND
CupError command_default(const char *component, const char *entry, const char *target_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    CupError err;

    err = entry_request_parse(component, entry, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_EXCLUSIVE);
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

    err = entry_request_resolve(context.has_manifest ? &context.manifest : NULL,
        component, context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform, request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = state_set_default(&context.state, component,
        context.host_platform, context.target_platform, request.resolved_entry);
    if (err != CUP_OK) {
        goto done;
    }

    err = state_save(&context.state);
    if (err != CUP_OK) {
        goto done;
    }

    printf("Default %s for host '%s', target '%s' set to ",
        component, context.host_platform, context.target_platform);
    entry_request_print(stdout, &request);
    printf(".\n");

done:
    command_context_end(&context);
    return err;
}

// CURRENT COMMAND
CupError command_current(const char *component, const char *target_override) {
    CommandContext context = {0};
    PackageIdentity package;
    CupError err;
    const char *default_entry;
    int is_stable = 0;

    err = registry_validate_component(component);
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

    command_context_try_manifest(&context);

    default_entry = state_get_default(&context.state, component,
        context.host_platform, context.target_platform);
    if (default_entry == NULL) {
        printf("No default set for component '%s' on host '%s', target '%s'.\n",
            component, context.host_platform, context.target_platform);
        err = CUP_OK;
        goto done;
    }

    err = package_identity_from_entry(&package, component,
        context.host_platform, context.target_platform, default_entry);
    if (err != CUP_OK) {
        err = CUP_ERR_INCONSISTENT_STATE;
        goto done;
    }

    err = command_require_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    if (context.has_manifest) {
        manifest_is_stable(&context.manifest, component, package.tool,
            package.host_platform, package.target_platform, package.version, &is_stable);
    }

    printf("Current %s default for host '%s', target '%s': %s%s\n",
        component, context.host_platform, context.target_platform,
        default_entry, is_stable ? " (stable)" : "");
    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

// INFO COMMAND
CupError command_info(const char *component, const char *entry, const char *target_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    PackageInfo info;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char info_path[MAX_PATH_LEN];

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

    err = entry_request_resolve(context.has_manifest ? &context.manifest : NULL,
        component, context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform, request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), &package);
    if (err != CUP_OK ||
        path_join(info_path, sizeof(info_path), install_path, CUP_INFO_FILENAME) != CUP_OK) {
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
