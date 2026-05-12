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
#include "system.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    MOVE_TMP_TO_INSTALL,
    MOVE_INSTALL_TO_TMP
} InstallMoveDirection;

typedef struct {
    char install_path[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];
} InstallTransaction;

typedef struct {
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    char resolved_release[MAX_NAME_LEN];
    char input_entry[MAX_ENTRY_LEN];
    char canonical_entry[MAX_ENTRY_LEN];
} EntryContext;

static void print_entry_resolution(FILE *stream, const EntryContext *ctx) {
    if (stream == NULL || ctx == NULL) {
        return;
    }

    if (strcmp(ctx->input_entry, ctx->canonical_entry) == 0) {
        fprintf(stream, "%s", ctx->input_entry);
        return;
    }

    fprintf(stream, "%s -> %s", ctx->input_entry, ctx->canonical_entry);
}

static CupError build_canonical_entry(char *buffer, size_t size, const char *tool, const char *version) {
    if (buffer == NULL || size == 0 || is_empty_string(tool) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "%s@%s", tool, version);
}

static CupError parse_entry_context(const char *component, const char *entry, EntryContext *ctx) {
    CupError err;

    if (ctx == NULL || is_empty_string(component) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(ctx, 0, sizeof(*ctx));

    err = checked_snprintf(ctx->input_entry, sizeof(ctx->input_entry), "%s", entry);
    if (err != CUP_OK) {
        return err;
    }

    err = split_once(ctx->input_entry, '@', ctx->tool, sizeof(ctx->tool), ctx->release, sizeof(ctx->release));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: invalid entry '%s'. Expected format '<tool>@<release>'.\n", ctx->input_entry);
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

    return CUP_OK;
}

static CupError resolve_entry_release(const char *component, const char *host_platform, const char *target_platform, 
    EntryContext *ctx) {
    CupError err;

    if (ctx == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(ctx->tool) || is_empty_string(ctx->release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = resolve_release(ctx->resolved_release, sizeof(ctx->resolved_release), component, ctx->tool, host_platform, target_platform, ctx->release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_canonical_entry(ctx->canonical_entry, sizeof(ctx->canonical_entry), ctx->tool, ctx->resolved_release);
    return err;
}

static CupError resolve_command_format(char *buffer, size_t size, const char *component, const char *host_platform, 
    const char *target_platform, const char *format_override, EntryContext *ctx) {
    CupError err;
    int format_supported;

    if (buffer == NULL || size == 0 || 
        is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!is_empty_string(format_override)) {
        err = is_format_supported(component, ctx->tool, host_platform, target_platform, format_override, &format_supported);
        if (err != CUP_OK) {
            return err;
        }

        if (!format_supported) {
            fprintf(stderr, "Error: archive format '%s' is not supported for tool '%s' on host '%s', target '%s'.\n", 
                format_override, ctx->tool, host_platform, target_platform);
            return CUP_ERR_INVALID_INPUT;
        }

        err = checked_snprintf(buffer, size, "%s", format_override);
        return err;
    }

    err = get_default_format(buffer, size, component, ctx->tool, host_platform, target_platform);
    return err;
}

static CupError resolve_command_platforms(char *host_platform, size_t host_size, char *target_platform, size_t target_size, 
    const char *target_override) {
    CupError err;

    if (host_platform == NULL || target_platform == NULL ||
        host_size == 0 || target_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_host_platform(host_platform, host_size);
    if (err != CUP_OK) {
        return err;
    }

    if (is_empty_string(target_override)) {
        err = checked_snprintf(target_platform, target_size, "%s", host_platform);
        return err;
    }
    
    err = validate_platform(target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(target_platform, target_size, "%s", target_override);
    return err;
}

static void cleanup_tmp_safely(const char *tmp_path) {
    CupError err;

    if (is_empty_string(tmp_path)) {
        return;
    }

    interrupt_reset();

    err = cleanup_tmp_path(tmp_path);
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

static CupError check_install_presence(const CupState *state, const char *component, const char *host_platform, 
    const char *target_platform, const EntryContext *ctx, int *in_state, int *on_disk) {
    CupError err;

    if (state == NULL || ctx == NULL || in_state == NULL || on_disk == NULL ||
        is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *in_state = (state_find_installed(state, component, host_platform, target_platform, ctx->canonical_entry) != -1);

    err = installation_exists(component, ctx->tool, host_platform, target_platform, ctx->resolved_release, on_disk);
    return err;
}

static CupError require_consistent_installed(const char *component, const char *host_platform, const char *target_platform, 
    const EntryContext *ctx, int in_state, int on_disk) {
    if (!in_state && !on_disk) {
        fprintf(stderr, "Error: '%s:", component);
        print_entry_resolution(stderr, ctx);
        fprintf(stderr, "' is not installed for host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_NOT_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:", component);
        print_entry_resolution(stderr, ctx);
        fprintf(stderr, "' on host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError require_not_installed(const char *component, const char *host_platform, const char *target_platform, 
    const EntryContext *ctx, int in_state, int on_disk) {
    if (in_state && on_disk) {
        fprintf(stderr, "Error: '%s:", component);
        print_entry_resolution(stderr, ctx);
        fprintf(stderr, "' is already installed for host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:", component);
        print_entry_resolution(stderr, ctx);
        fprintf(stderr, "' on host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError perform_install(const char *tmp_path, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *version, const char *archive_format) {
    CupError err;
    char package_path[MAX_PATH_LEN];

    if (is_empty_string(tmp_path) || is_empty_string(component) || is_empty_string(tool) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(version) || is_empty_string(archive_format)){
        return CUP_ERR_INVALID_INPUT;
    }

    print_step("Fetching package...");
    err = fetch_package(package_path, sizeof(package_path), component, tool, host_platform, target_platform, version, archive_format);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Extracting package...");
    err = extract_archive(package_path, tmp_path);
    return err;
}

static CupError move_install_transaction(InstallTransaction *tx, InstallMoveDirection direction) {
    CupError err;

    if (tx == NULL || is_empty_string(tx->install_path) || is_empty_string(tx->tmp_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (direction == MOVE_TMP_TO_INSTALL) {
        err = commit_path(tx->tmp_path, tx->install_path);
        return err;
    }

    err = commit_path(tx->install_path, tx->tmp_path);
    return err;
}

static void clear_tmp_path(InstallTransaction *tx) {
    if (tx == NULL) {
        return;
    }

    tx->tmp_path[0] = '\0';
}

static void cleanup_tmp_transaction(InstallTransaction *tx) {
    if (tx == NULL || is_empty_string(tx->tmp_path)) {
        return;
    }

    cleanup_tmp_safely(tx->tmp_path);
    clear_tmp_path(tx);
}

CupError handle_list(const char *target_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int printed = 0;
    int has_annotation;
    int is_default;
    int is_stable;
    int manifest_available;
    int on_disk;
    size_t i;

    err = resolve_command_platforms(host_platform, sizeof(host_platform), target_platform, sizeof(target_platform), target_override);
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

    for (i = 0; i < state.installed_count; ++i) {
        const char *default_entry;

        if (strcmp(state.installed[i].host_platform, host_platform) != 0 ||
            strcmp(state.installed[i].target_platform, target_platform) != 0) {
            continue;
        }

        if (!printed) {
            printf("Installed components for host '%s', target '%s':\n", host_platform, target_platform);
            printed = 1;
        }
        
        printf("- %s:%s", state.installed[i].component, state.installed[i].entry);

        err = parse_entry_context(state.installed[i].component, state.installed[i].entry, &ctx);
        if (err != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        err = build_canonical_entry(ctx.canonical_entry, sizeof(ctx.canonical_entry), ctx.tool, ctx.release);
        if (err != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        err = installation_exists(state.installed[i].component, ctx.tool, host_platform, target_platform, ctx.release, &on_disk);
        if (err != CUP_OK) {
            printf(" (could not inspect)\n");
            continue;
        }

        if (!on_disk) {
            printf(" (missing on disk)\n");
            continue;
        }

        default_entry = state_get_default(&state, state.installed[i].component, host_platform, target_platform);
        is_default = (default_entry != NULL && strcmp(default_entry, ctx.canonical_entry) == 0);
        manifest_available = 1;
        is_stable = 0;

        err = is_stable_version(state.installed[i].component, ctx.tool, host_platform, target_platform, ctx.release, &is_stable);
        if (err != CUP_OK) {
            manifest_available = 0;
            is_stable = 0;
        }

        has_annotation = 0;

        if (is_default || is_stable || !manifest_available) {
            printf(" (");

            if (is_default) {
                printf("default");
                has_annotation = 1;
            }

            if (is_stable) {
                if (has_annotation) {
                    printf(", ");
                }

                printf("stable");
                has_annotation = 1;
            }

            if (!manifest_available) {
                if (has_annotation) {
                    printf(", ");
                }

                printf("manifest unavailable");
            }

            printf(")");
        }

        printf("\n");
    }

    if (!printed) {
        printf("No components installed for host '%s', target '%s'.\n", host_platform, target_platform);
    }

    return CUP_OK;
}

CupError handle_install(const char *component, const char *entry, const char *target_override, const char *format_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    CupError rollback_err;
    InstallTransaction tx;
    char state_file[MAX_PATH_LEN];
    char archive_format[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int version_available;
    int in_state;
    int on_disk;

    memset(&tx, 0, sizeof(tx));
    interrupt_setup();

    print_step("Validating request...");
    err = resolve_command_platforms(host_platform, sizeof(host_platform), target_platform, sizeof(target_platform), target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = parse_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_release(component, host_platform, target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = is_version_available(component, ctx.tool, host_platform, target_platform, ctx.resolved_release, &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr, "Error: version '%s' is not available for tool '%s' on host '%s', target '%s'.\n",
            ctx.resolved_release, ctx.tool, host_platform, target_platform);
        return CUP_ERR_INVALID_RELEASE;
    }

    print_step("Resolving package format...");
    err = resolve_command_format(archive_format, sizeof(archive_format), component, host_platform, target_platform, format_override, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    print_step("Loading state...");
    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Checking existing installation...");
    err = check_install_presence(&state, component, host_platform, target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_not_installed(component, host_platform, target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_dir(tx.tmp_path, sizeof(tx.tmp_path), "install", component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = perform_install(tx.tmp_path, component, ctx.tool, host_platform, target_platform, ctx.resolved_release, archive_format);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Validating installation...");
    err = validate_install(tx.tmp_path);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Preparing final installation directories...");
    err = ensure_component_base_dirs(component, ctx.tool, host_platform, target_platform);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(tx.install_path, sizeof(tx.install_path), component, ctx.tool, host_platform, target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    print_step("Committing installation...");
    err = move_install_transaction(&tx, MOVE_TMP_TO_INSTALL);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = state_add_installed(&state, component, host_platform, target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_install_transaction(&tx, MOVE_INSTALL_TO_TMP);
        if (rollback_err != CUP_OK) {
            cleanup_tmp_transaction(&tx);
            fprintf(stderr, "Error: failed to add install to state and rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        cleanup_tmp_transaction(&tx);
        fprintf(stderr, "Error: failed to add install to state. Installation rolled back.\n");
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        rollback_err = move_install_transaction(&tx, MOVE_INSTALL_TO_TMP);
        if (rollback_err != CUP_OK) {
            cleanup_tmp_transaction(&tx);
            fprintf(stderr, "Error: state save failed and rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        cleanup_tmp_transaction(&tx);
        fprintf(stderr, "Error: state save failed. Installation rolled back.\n");
        return CUP_ERR_STATE_SAVE;
    }

    clear_tmp_path(&tx);

    printf("Installed %s ", component);
    print_entry_resolution(stdout, &ctx);
    printf(" for host '%s', target '%s' successfully.\n", host_platform, target_platform);
    return CUP_OK;
}

CupError handle_remove(const char *component, const char *entry, const char *target_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    CupError rollback_err;
    InstallTransaction tx;
    char state_file[MAX_PATH_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int in_state;
    int on_disk;

    memset(&tx, 0, sizeof(tx));
    interrupt_setup();

    print_step("Validating request...");
    err = resolve_command_platforms(host_platform, sizeof(host_platform), target_platform, sizeof(target_platform), target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = parse_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_release(component, host_platform, target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    print_step("Loading state...");
    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Checking installed package...");
    err = check_install_presence(&state, component, host_platform, target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, host_platform, target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_dir(tx.tmp_path, sizeof(tx.tmp_path), "remove", component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(tx.install_path, sizeof(tx.install_path), component, ctx.tool, host_platform, target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = system_remove_directory(tx.tmp_path);
    if (err != CUP_OK) {
        clear_tmp_path(&tx);
        return err;
    }

    print_step("Staging removal...");
    err = move_install_transaction(&tx, MOVE_INSTALL_TO_TMP);
    if (err != CUP_OK) {
        cleanup_tmp_transaction(&tx);
        return err;
    }

    err = state_remove_installed(&state, component, host_platform, target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_install_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_tmp_path(&tx);
        return err;
    }

    err = state_remove_default_if_matches(&state, component, host_platform, target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_install_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_tmp_path(&tx);
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        rollback_err = move_install_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_tmp_path(&tx);
        fprintf(stderr, "Error: could not save state after remove.\n");
        return err;
    }

    print_step("Cleaning temporary files...");
    cleanup_tmp_transaction(&tx);

    printf("Removed %s ", component);
    print_entry_resolution(stdout, &ctx);
    printf(" for host '%s', target '%s' successfully.\n", host_platform, target_platform);
    return CUP_OK;
}

CupError handle_default(const char *component, const char *entry, const char *target_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int in_state;
    int on_disk;

    print_step("Validating request...");
    err = resolve_command_platforms(host_platform, sizeof(host_platform), target_platform, sizeof(target_platform), target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = parse_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_release(component, host_platform, target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    print_step("Loading state...");
    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Checking installed package...");
    err = check_install_presence(&state, component, host_platform, target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, host_platform, target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = state_set_default(&state, component, host_platform, target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not set default for component '%s'.\n", component);
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    printf("Default %s for host '%s', target '%s' set to ", component, host_platform, target_platform);
    print_entry_resolution(stdout, &ctx);
    printf(".\n");
    return CUP_OK;
}

CupError handle_current(const char *component, const char *target_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    const char *default_entry;
    char state_file[MAX_PATH_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int is_stable;
    int in_state;
    int on_disk;

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_command_platforms(host_platform, sizeof(host_platform), target_platform, sizeof(target_platform), target_override);
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

    default_entry = state_get_default(&state, component, host_platform, target_platform);
    if (default_entry == NULL) {
        printf("No default set for component '%s' on host '%s', target '%s'.\n", component, host_platform, target_platform);
        return CUP_OK;
    }

    err = parse_entry_context(component, default_entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Resolving release...");
    err = resolve_entry_release(component, host_platform, target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&state, component, host_platform, target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (!in_state || !on_disk) {
        fprintf(stderr, "Error: default for component '%s' on host '%s', target '%s' points to an inconsistent installation '", component, host_platform, target_platform);
        print_entry_resolution(stderr, &ctx);
        fprintf(stderr, "'.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }

    is_stable = 0;

    err = is_stable_version(component, ctx.tool, host_platform, target_platform, ctx.resolved_release, &is_stable);
    if (err != CUP_OK) {
        is_stable = 0;
    }

    printf("Current %s default for host '%s', target '%s': %s", component, host_platform, target_platform, ctx.input_entry);
    if (is_stable) {
        printf(" (stable)");
    }
    printf("\n");
    return CUP_OK;
}