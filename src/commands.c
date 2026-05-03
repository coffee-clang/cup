#include "commands.h"
#include "state.h"
#include "filesystem.h"
#include "fetch.h"
#include "extract.h"
#include "manifest.h"
#include "registry.h"
#include "util.h"
#include "interrupt.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    char resolved_release[MAX_NAME_LEN];
    char canonical_entry[MAX_ENTRY_LEN];
} EntryContext;

static void print_step(const char *message) {
    if (message == NULL) {
        return;
    }

    printf("==> %s\n", message);
}

static int is_valid_entry(const char *entry) {
    const char *at;

    if (entry == NULL || entry[0] == '\0') {
        return 0;
    }

    at = strchr(entry, '@');
    if (at == NULL) {
        return 0;
    }

    if (at == entry) {
        return 0;
    }

    if (*(at + 1) == '\0') {
        return 0;
    }

    if (strchr(at + 1, '@') != NULL) {
        return 0;
    }

    return 1;
}

static CupError split_entry(const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *at;
    size_t tool_len;
    size_t release_len;

    if (entry == NULL || tool == NULL || release == NULL ||
        entry[0] == '\0' || tool_size == 0 || release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    at = strchr(entry, '@');
    if (at == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    tool_len = (size_t)(at - entry);
    release_len = strlen(at + 1);

    if (tool_len == 0 || release_len == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (tool_len >= tool_size || release_len >= release_size) {
        return CUP_ERR_INVALID_INPUT;
    }

    memcpy(tool, entry, tool_len);
    tool[tool_len] = '\0';

    memcpy(release, at + 1, release_len);
    release[release_len] = '\0';

    return CUP_OK;
}

static CupError build_canonical_entry(char *buffer, size_t size, const char *tool, const char *resolved_release) {
    if (buffer == NULL || tool == NULL || resolved_release == NULL ||
        size == 0 || tool[0] == '\0' || resolved_release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "%s@%s", tool, resolved_release);
}

static CupError resolve_entry_context(const char *component, const char *platform, const char *entry, EntryContext *ctx) {
    CupError err;

    if (component == NULL || platform == NULL || entry == NULL || ctx == NULL ||
        component[0] == '\0' || platform[0] == '\0' || entry[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (!is_valid_entry(entry)) {
        fprintf(stderr, "Error: invalid entry format. Use <tool>@<release>.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_entry(entry, ctx->tool, sizeof(ctx->tool), ctx->release, sizeof(ctx->release));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: invalid entry '%s'.\n", entry);
        return err;
    }

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_tool_for_component(component, ctx->tool);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_release(ctx->resolved_release, sizeof(ctx->resolved_release), component, ctx->tool, platform, ctx->release);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not resolve release '%s' for tool '%s'.\n", ctx->release, ctx->tool);
        return err;
    }

    err = build_canonical_entry(ctx->canonical_entry, sizeof(ctx->canonical_entry), ctx->tool, ctx->resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

static CupError resolve_command_format(char *buffer, size_t size, const char *component, const char *platform, const char *format_override, EntryContext *ctx) {
    CupError err;
    int format_supported;

    if (buffer == NULL || component == NULL || ctx == NULL ||
        size == 0 || component[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (format_override != NULL && format_override[0] != '\0') {
        err = is_format_supported(component, ctx->tool, platform, format_override, &format_supported);
        if (err != CUP_OK) {
            return err;
        }

        if (!format_supported) {
            fprintf(stderr, "Error: archive format '%s' is not supported for tool '%s'.\n", format_override, ctx->tool);
            return CUP_ERR_INVALID_INPUT;
        }

        err = checked_snprintf(buffer, size, "%s", format_override);
        return err;
    }

    err = get_default_format(buffer, size, component, ctx->tool, platform);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not determine default archive format for tool '%s'.\n", ctx->tool);
        return err;
    }

    return CUP_OK;
}

static CupError resolve_command_platform(char *buffer, size_t size, const char *platform_override) {
    CupError err;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (platform_override != NULL && platform_override[0] != '\0') {
        err = validate_platform(platform_override);
        if (err != CUP_OK) {
            return err;
        }

        err = checked_snprintf(buffer, size, "%s", platform_override);
        return err;
    }
    
    err = get_host_platform(buffer, size);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not detect host platform.\n");
        return err;
    }

    return CUP_OK;
}

static void cleanup_tmp_safely(const char *tmp_path) {
    CupError err;

    if (tmp_path == NULL || tmp_path[0] == '\0') {
        return;
    }

    interrupt_reset();

    err = cleanup_tmp_install(tmp_path);
    if (err == CUP_ERR_INTERRUPT) {
        fprintf(stderr, "Warning: cleanup was interrupted.\n");
    } else if (err != CUP_OK) {
        fprintf(stderr, "Warning: temporary files could not be fully cleaned.\n");
    }
}

static CupError stop_if_interrupted(const char *tmp_path) {
    if (!interrupt_requested()) {
        return CUP_OK;
    }

    cleanup_tmp_safely(tmp_path);
    return CUP_ERR_INTERRUPT;
}

static CupError check_install_presence(const CupState *state, const char *component, const char *platform, const EntryContext *ctx, int *in_state, int *on_disk) {
    CupError err;

    if (state == NULL || component == NULL || platform == NULL || ctx == NULL || 
        in_state == NULL || component[0] == '\0' || platform[0] == '\0' || on_disk == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *in_state = (state_find_installed(state, component, platform, ctx->canonical_entry) != -1);

    err = installation_exists(component, ctx->tool, platform, ctx->resolved_release, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

static CupError require_consistent_installed(const char *component, const char *entry, int in_state, int on_disk) {
    if (!in_state && !on_disk) {
        fprintf(stderr, "Error: '%s:%s' is not installed.\n", component, entry);
        return CUP_ERR_NOT_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:%s'.\n", component, entry);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError require_not_installed(const char *component, const char *entry, int in_state, int on_disk) {
    if (in_state && on_disk) {
        fprintf(stderr, "Error: '%s:%s' is already installed.\n", component, entry);
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:%s'.\n", component, entry);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError perform_install(const char *tmp_path, const char *component, const char *tool, const char *platform, const char *release, const char *archive_format) {
    CupError err;
    char package_path[MAX_PATH_LEN];

    if (tmp_path == NULL || component == NULL || tool == NULL || platform == NULL || release == NULL || archive_format == NULL ||
        tmp_path[0] == '\0' || component[0] == '\0' || tool[0] == '\0' || platform[0] == '\0' || release[0] == '\0' || archive_format[0] == '\0'){
        return CUP_ERR_INVALID_INPUT;
    }

    err = fetch_package(package_path, sizeof(package_path), component, tool, platform, release, archive_format);
    if (err != CUP_OK) {
        return err;
    }

    err = extract_archive_to_tmp(package_path, tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = write_component_info_at_path(tmp_path, component, tool, platform, release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

static CupError rollback_committed_install(const char *component, const char *platform, const EntryContext *ctx) {
    CupError err;
    char install_path[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];

    if (component == NULL || platform == NULL || ctx == NULL || 
        component[0] == '\0' || platform[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_install_path(install_path, sizeof(install_path), component, ctx->tool, platform, ctx->resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_remove_dir(tmp_path, sizeof(tmp_path), component, ctx->tool, ctx->resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = commit_path(install_path, tmp_path);
    if (err != CUP_OK) {
        cleanup_tmp_safely(tmp_path);
        return err;
    }

    cleanup_tmp_safely(tmp_path);
    return CUP_OK;
}

CupError handle_list(const char *platform_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    char platform[MAX_PLATFORM_LEN];
    int printed = 0;
    int is_stable;
    int on_disk;
    size_t i;

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_command_platform(platform, sizeof(platform), platform_override);
    if (err != CUP_OK) {
        return err;
    }

    printf("Installed components for '%s':\n", platform);
    for (i = 0; i < state.installed_count; ++i) {
        const char *default_entry;

        if(strcmp(state.installed[i].platform, platform) != 0) {
            continue;
        }

        is_stable = 0;
        printed = 1;
        
        printf("- %s:%s", state.installed[i].component, state.installed[i].entry);

        err = resolve_entry_context(state.installed[i].component, platform, state.installed[i].entry, &ctx);
        if (err != CUP_OK) {
            printf(" (invalid state entry)");
            printf("\n");
            continue;
        }

        err = installation_exists(state.installed[i].component, ctx.tool, platform, ctx.resolved_release, &on_disk);
        if (err != CUP_OK) {
            printf(" (could not inspect)");
            printf("\n");
            continue;
        }

        if (!on_disk) {
            printf(" (missing on disk)");
        }

        default_entry = state_get_default(&state, state.installed[i].component, platform);
        if (default_entry != NULL && 
            strcmp(default_entry, state.installed[i].entry) == 0) {
            printf(" (default)");
        }

        err = is_stable_release(state.installed[i].component, ctx.tool, platform, ctx.resolved_release, &is_stable);
        if (err != CUP_OK) {
            is_stable = 0;
        }

        if (is_stable) {
            printf(" (stable)");
        }

        printf("\n");
    }

    if (!printed) {
        printf("No components installed for platform '%s'.\n", platform);
    }

    return CUP_OK;
}

CupError handle_install(const char *component, const char *entry, const char *format_override, const char *platform_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    CupError rollback_err;
    char state_file[MAX_PATH_LEN];
    char archive_format[MAX_NAME_LEN];
    char platform[MAX_PLATFORM_LEN];
    char tmp_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int version_available;
    int in_state;
    int on_disk;

    tmp_path[0] = '\0';
    interrupt_setup();

    err = resolve_command_platform(platform, sizeof(platform), platform_override);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_context(component, platform, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_command_format(archive_format, sizeof(archive_format), component, platform, format_override, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = is_version_available(component, ctx.tool, platform, ctx.resolved_release, &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr, "Error: version '%s' is not available for tool '%s'.\n", ctx.resolved_release, ctx.tool);
        return CUP_ERR_INVALID_RELEASE;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&state, component, platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_not_installed(component, entry, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_install_dir(tmp_path, sizeof(tmp_path), component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Fetching and installing package...");
    err = perform_install(tmp_path, component, ctx.tool, platform, ctx.resolved_release, archive_format);
    if (err != CUP_OK) {
        cleanup_tmp_safely(tmp_path);
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Validating installation...");
    err = validate_install(tmp_path);
    if (err != CUP_OK) {
        cleanup_tmp_safely(tmp_path);
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Preparing final installation directories...");
    err = ensure_component_base_dirs(component, ctx.tool, platform);
    if (err != CUP_OK) {
        cleanup_tmp_safely(tmp_path);
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(install_path, sizeof(install_path), component, ctx.tool, platform, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Committing installation...");
    err = commit_path(tmp_path, install_path);
    if (err != CUP_OK) {
        return err;
    }

    tmp_path[0] = '\0';

    err = state_add_installed(&state, component, platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = rollback_committed_install(component, platform, &ctx);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: failed to add install to state and rollback failed for '%s:%s'.\n", component, entry);
            return CUP_ERR_ROLLBACK;
        }

        fprintf(stderr, "Error: failed to add install to state. Installation rolled back.\n");
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        rollback_err = rollback_committed_install(component, platform, &ctx);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: state save failed and rollback failed for '%s:%s'.\n", component, entry);
            return CUP_ERR_ROLLBACK;
        }

        fprintf(stderr, "Error: state save failed. Installation rolled back.\n");
        return CUP_ERR_STATE_SAVE;
    }

    printf("Installed %s %s successfully.\n", component, entry);
    return CUP_OK;
}

CupError handle_remove(const char *component, const char *entry, const char *platform_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    CupError rollback_err;
    char state_file[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];
    char platform[MAX_PLATFORM_LEN];
    int in_state;
    int on_disk;

    tmp_path[0] = '\0';
    interrupt_setup();

    err = resolve_command_platform(platform, sizeof(platform), platform_override);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_context(component, platform, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&state, component, platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, entry, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_remove_dir(tmp_path, sizeof(tmp_path), component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = ensure_component_base_dirs(component, ctx.tool, platform);
    if (err != CUP_OK) {
        cleanup_tmp_safely(tmp_path);
        return err;
    }

    err = stop_if_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(install_path, sizeof(install_path), component, ctx.tool, platform, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Committing remove...");
    err = commit_path(install_path, tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = state_remove_installed(&state, component, platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        return err;
    }

    err = state_remove_default_if_matches(&state, component, platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        rollback_err = commit_path(tmp_path, install_path);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, entry);
            return CUP_ERR_ROLLBACK;
        }

        tmp_path[0] = '\0';

        fprintf(stderr, "Error: could not save state after remove.\n");
        return err;
    }

    install_path[0] = '\0';

    print_step("Cleaning removed files...");
    cleanup_tmp_safely(tmp_path);
    tmp_path[0] = '\0';

    printf("Removed %s %s successfully.\n", component, entry);
    return CUP_OK;
}

CupError handle_default(const char *component, const char *entry, const char *platform_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    char platform[MAX_PLATFORM_LEN];
    int in_state;
    int on_disk;

    err = resolve_command_platform(platform, sizeof(platform), platform_override);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_entry_context(component, platform, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&state, component, platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, entry, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = state_set_default(&state, component, platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not set default for component '%s'.\n", component);
        return err;
    }

    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    printf("Default %s set to '%s'.\n", component, entry);
    return CUP_OK;
}

CupError handle_current(const char *component, const char *platform_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    const char *default_entry;
    char state_file[MAX_PATH_LEN];
    char platform[MAX_PLATFORM_LEN];
    int is_stable;
    int in_state;
    int on_disk;

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_command_platform(platform, sizeof(platform), platform_override);
    if (err != CUP_OK) {
        return err;
    }

    default_entry = state_get_default(&state, component, platform);
    if (default_entry == NULL) {
        printf("No default set for component '%s'.\n", component);
        return CUP_OK;
    }

    err = resolve_entry_context(component, platform, default_entry, &ctx);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: default for component '%s' is invalid.\n", component);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = check_install_presence(&state, component, platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (!in_state || !on_disk) {
        fprintf(stderr, "Error: default for component '%s' points to an inconsistent installation '%s'.\n", component, default_entry);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    is_stable = 0;

    err = is_stable_release(component, ctx.tool, platform, ctx.resolved_release, &is_stable);
    if (err != CUP_OK) {
        is_stable = 0;
    }

    if (is_stable) {
        printf("Current %s default for %s: %s (stable)\n", component, platform, default_entry);
    } else {
        printf("Current %s default for %s: %s\n", component, platform, default_entry);
    }

    return CUP_OK;
}