#include "commands.h"

#include "extract.h"
#include "fetch.h"
#include "filesystem.h"
#include "interrupt.h"
#include "manifest.h"
#include "info.h"
#include "entry.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "state.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// TYPES
typedef enum {
    MOVE_TMP_TO_INSTALL,
    MOVE_INSTALL_TO_TMP
} MoveDirection;

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

typedef struct {
    CupState state;
    Manifest manifest;
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
} CommandContext;

typedef struct {
    int issues;
    int warnings;
} DoctorReport;

// ENTRY HELPERS
static void print_resolved_entry(FILE *stream, const EntryContext *ctx) {
    if (stream == NULL || ctx == NULL) {
        return;
    }

    if (strcmp(ctx->input_entry, ctx->canonical_entry) == 0) {
        fprintf(stream, "%s", ctx->input_entry);
        return;
    }

    fprintf(stream, "%s -> %s", ctx->input_entry, ctx->canonical_entry);
}

static CupError prepare_entry_context(const char *component, const char *entry, EntryContext *ctx) {
    CupError err;

    if (is_empty_string(component) || is_empty_string(entry) || ctx == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(ctx, 0, sizeof(*ctx));

    err = checked_snprintf(ctx->input_entry, sizeof(ctx->input_entry), "%s", entry);
    if (err != CUP_OK) {
        return err;
    }

    err = parse_entry(entry, ctx->tool, sizeof(ctx->tool), ctx->release, sizeof(ctx->release));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: invalid entry '%s'. Expected '<tool>@<release>'.\n", entry);
        return err;
    }
    
    return CUP_OK;
}

static CupError resolve_entry_release(const Manifest *manifest, const char *component, const char *host_platform, const char *target_platform, EntryContext *ctx) {
    CupError err;

    if (ctx == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(ctx->tool) || is_empty_string(ctx->release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (is_stable_release(ctx->release) != 0) {
        err = resolve_stable_release(manifest, ctx->resolved_release, sizeof(ctx->resolved_release), component, ctx->tool, host_platform, target_platform);
        if (err != CUP_OK) {
            return err;
        }
    } else {
        err = checked_snprintf(ctx->resolved_release, sizeof(ctx->resolved_release), "%s", ctx->release);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = build_entry(ctx->canonical_entry, sizeof(ctx->canonical_entry), ctx->tool, ctx->resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// COMMAND CONTEXT / OPTIONS / PLATFORM
static CupError resolve_command_format(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *host_platform, const char *target_platform, const char *format_override, const EntryContext *ctx) {
    CupError err;
    int format_supported;

    if (buffer == NULL || size == 0 || ctx == NULL ||
        is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!is_empty_string(format_override)) {
        err = is_format_supported(manifest, component, ctx->tool, host_platform, target_platform, format_override, &format_supported);
        if (err != CUP_OK) {
            return err;
        }

        if (!format_supported) {
            fprintf(stderr, "Error: archive format '%s' is not available for tool '%s' on host '%s', target '%s'.\n",
                format_override, ctx->tool, host_platform, target_platform);
            return CUP_ERR_NOT_AVAILABLE;
        }

        err = checked_snprintf(buffer, size, "%s", format_override);
        return err;
    }

    err = get_default_format(manifest, buffer, size, component, ctx->tool, host_platform, target_platform);
    return err;
}

static CupError resolve_command_platforms(char *host_platform, size_t host_size, char *target_platform, size_t target_size, const char *target_override) {
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

/*
 * Load the full context required by normal commands.
 * Recovery/diagnostic commands such as doctor and repair intentionally
 * perform their own checks instead of using this function, so they can
 * report corrupted state or manifest files.
 */
static CupError load_command_context(CommandContext *ctx, const char *target_override) {
    CupError err;

    if (ctx == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(ctx, 0, sizeof(*ctx));

    err = resolve_command_platforms(ctx->host_platform, sizeof(ctx->host_platform), ctx->target_platform, 
        sizeof(ctx->target_platform), target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&ctx->state);
    if (err != CUP_OK) {
        return err;
    }

    err = manifest_load(&ctx->manifest);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// INTERRUPT / CLEANUP HELPERS
static void cleanup_tmp_safely(const char *tmp_path) {
    CupError err;

    if (is_empty_string(tmp_path)) {
        return;
    }

    interrupt_reset();

    err = cleanup_tmp_path(tmp_path);
    if (err == CUP_ERR_INTERRUPT) {
        fprintf(stderr, "\nWarning: cleanup was interrupted.\n");
    } else if (err != CUP_OK) {
        fprintf(stderr, "\nWarning: temporary files could not be fully cleaned.\n");
    }
}

static CupError stop_if_interrupted(const char *tmp_path) {
    if (!interrupt_requested()) {
        return CUP_OK;
    }

    cleanup_tmp_safely(tmp_path);
    return CUP_ERR_INTERRUPT;
}

// INSTALL / REMOVE TRANSACTION HELPERS
static CupError check_install_presence(const CupState *state, const char *component, const char *host_platform, const char *target_platform, const EntryContext *ctx, int *in_state, int *on_disk) {
    CupError err;

    if (state == NULL || ctx == NULL || in_state == NULL || on_disk == NULL ||
        is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *in_state = (state_find_installed(state, component, host_platform, target_platform, ctx->canonical_entry) != -1);

    err = install_dir_exists(component, ctx->tool, host_platform, target_platform, ctx->resolved_release, on_disk);
    return err;
}

static CupError require_consistent_installed(const char *component, const char *host_platform, const char *target_platform, const EntryContext *ctx, int in_state, int on_disk) {
    if (!in_state && !on_disk) {
        fprintf(stderr, "Error: '%s:", component);
        print_resolved_entry(stderr, ctx);
        fprintf(stderr, "' is not installed for host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_NOT_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:", component);
        print_resolved_entry(stderr, ctx);
        fprintf(stderr, "' on host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError require_not_installed(const char *component, const char *host_platform, const char *target_platform, const EntryContext *ctx, int in_state, int on_disk) {
    if (in_state && on_disk) {
        fprintf(stderr, "Error: '%s:", component);
        print_resolved_entry(stderr, ctx);
        fprintf(stderr, "' is already installed for host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:", component);
        print_resolved_entry(stderr, ctx);
        fprintf(stderr, "' on host '%s', target '%s'.\n", host_platform, target_platform);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

static CupError stage_package_install(const Manifest *manifest, const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, const char *archive_format) {
    CupError err;
    char package_path[MAX_PATH_LEN];
    char package_url[MAX_MANIFEST_URL_LEN];

    if (manifest == NULL || is_empty_string(tmp_path) || is_empty_string(component) || 
        is_empty_string(tool) || is_empty_string(host_platform) ||  is_empty_string(target_platform) || 
        is_empty_string(version) || is_empty_string(archive_format)){
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_download_url(manifest, package_url, sizeof(package_url), component, tool, host_platform, target_platform, version, archive_format);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Fetching package...\n");
    err = fetch_package(package_path, sizeof(package_path), package_url, component, tool, version);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Extracting package...\n");
    err = extract_archive(package_path, tmp_path);
    return err;
}

static CupError move_transaction(InstallTransaction *tx, MoveDirection direction) {
    CupError err;

    if (tx == NULL || is_empty_string(tx->install_path) || is_empty_string(tx->tmp_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (direction == MOVE_TMP_TO_INSTALL) {
        err = system_rename_path(tx->tmp_path, tx->install_path);
    } else {
        err = system_rename_path(tx->install_path, tx->tmp_path);
    }

    if (err != CUP_OK) {
        return CUP_ERR_COMMIT;
    }
    
    return CUP_OK;
}

static void clear_transaction_tmp(InstallTransaction *tx) {
    if (tx == NULL) {
        return;
    }

    tx->tmp_path[0] = '\0';
}

static void cleanup_transaction(InstallTransaction *tx) {
    if (tx == NULL || is_empty_string(tx->tmp_path)) {
        return;
    }

    cleanup_tmp_safely(tx->tmp_path);
    clear_transaction_tmp(tx);
}

// DOCTOR HELPERS
static CupError doctor_check_structure(DoctorReport *report) {
    CupError err;
    size_t missing_count;

    if (report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = check_cup_structure(&missing_count);
    if (err != CUP_OK) {
        report->warnings++;
        fprintf(stderr, "Warning: could not inspect cup directory structure.\n");
        return CUP_OK;
    }

    if (missing_count > 0) {
        report->warnings += (int)missing_count;
    } else {
        printf("OK: cup directory structure is valid.\n");
    }

    return CUP_OK;
}

static CupError doctor_check_state(CupState *state, DoctorReport *report) {
    CupError err;

    if (state == NULL || report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = state_load(state);
    if (err != CUP_OK) {
        fprintf(stderr, "Issue: state file is missing, unreadable, or malformed.\n");
        report->issues++;
        return CUP_OK;
    }

    printf("OK: state file is valid.\n");
    return CUP_OK;
}

static CupError doctor_check_manifest(Manifest *manifest, DoctorReport *report) {
    CupError err;

    if (manifest == NULL || report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = manifest_load(manifest);
    if (err != CUP_OK) {
        fprintf(stderr, "Issue: package manifest is missing, unreadable, or malformed.\n");
        report->issues++;
        return CUP_OK;
    }

    printf("OK: package manifest is readable.\n");
    return CUP_OK;
}

static CupError doctor_check_info_files(const CommandContext *command, DoctorReport *report) {
    CupError err;
    EntryContext ctx;
    char install_path[MAX_PATH_LEN];
    size_t i;

    if (command == NULL || report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < command->state.installed_count; ++i) {
        const StateEntry *entry = &command->state.installed[i];

        err = parse_entry(entry->entry, ctx.tool, sizeof(ctx.tool), ctx.release, sizeof(ctx.release));
        if (err != CUP_OK) {
            fprintf(stderr, "Issue: installed state entry '%s' is malformed.\n", entry->entry);
            report->issues++;
            continue;
        }

        err = build_install_path(install_path, sizeof(install_path), entry->component, ctx.tool, entry->host_platform, 
            entry->target_platform, ctx.release);
        if (err != CUP_OK) {
            fprintf(stderr, "Issue: could not build install path for '%s'.\n", entry->entry);
            report->issues++;
            continue;
        }

        err = validate_installation_metadata(install_path, entry->component, ctx.tool, entry->host_platform, entry->target_platform, ctx.release);
        if (err != CUP_OK) {
            fprintf(stderr, "Issue: installed package metadata is invalid for '%s'.\n", entry->entry);
            report->issues++;
            continue;
        }
    }

    if (command->state.installed_count > 0) {
        printf("OK: installed package info files are valid.\n");
    }

    return CUP_OK;
}

static CupError doctor_check_installed_entries(const CommandContext *command, DoctorReport *report) {
    EntryContext ctx;
    CupError err;
    int on_disk;
    size_t i;

    if (command == NULL || report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < command->state.installed_count; ++i) {
        err = prepare_entry_context(command->state.installed[i].component, command->state.installed[i].entry, &ctx);
        if (err != CUP_OK) {
            report->issues++;
            fprintf(stderr, "Error: invalid state entry '%s'.\n", command->state.installed[i].entry);
            continue;
        }

        err = build_entry(ctx.canonical_entry, sizeof(ctx.canonical_entry), ctx.tool, ctx.release);
        if (err != CUP_OK) {
            report->issues++;
            fprintf(stderr, "Error: could not parse state entry '%s'.\n", command->state.installed[i].entry);
            continue;
        }

        err = install_dir_exists(command->state.installed[i].component, ctx.tool, command->state.installed[i].host_platform,
            command->state.installed[i].target_platform, ctx.release, &on_disk);
        if (err != CUP_OK) {
            report->issues++;
            fprintf(stderr, "Error: could not inspect install directory for '%s:%s'.\n",
                command->state.installed[i].component, command->state.installed[i].entry);
            continue;
        }

        if (!on_disk) {
            report->issues++;
            fprintf(stderr, "Error: state entry '%s:%s' for host '%s', target '%s' is missing on disk.\n",
                command->state.installed[i].component, command->state.installed[i].entry,
                command->state.installed[i].host_platform, command->state.installed[i].target_platform);
        }
    }

    return CUP_OK;
}

static CupError doctor_check_manifest_for_entries(const CommandContext *command, DoctorReport *report) {
    EntryContext ctx;
    CupError err;
    int is_available;
    size_t i;

    if (command == NULL || report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < command->state.installed_count; ++i) {
        err = prepare_entry_context(command->state.installed[i].component, command->state.installed[i].entry, &ctx);
        if (err != CUP_OK) {
            continue;
        }

        err = is_version_available(&command->manifest, command->state.installed[i].component, ctx.tool, 
            command->state.installed[i].host_platform, command->state.installed[i].target_platform, ctx.release, &is_available);
        if (err != CUP_OK) {
            report->warnings++;
            fprintf(stderr, "Warning: manifest information is unavailable for '%s:%s' on host '%s', target '%s'.\n",
                command->state.installed[i].component, command->state.installed[i].entry,
                command->state.installed[i].host_platform, command->state.installed[i].target_platform);
            continue;
        }

        if (!is_available) {
            report->warnings++;
            fprintf(stderr, "Warning: installed version '%s:%s' is not listed in the current manifest for host '%s', target '%s'.\n",
                command->state.installed[i].component, command->state.installed[i].entry,
                command->state.installed[i].host_platform, command->state.installed[i].target_platform);
        }
    }

    return CUP_OK;
}

static CupError doctor_check_tmp(DoctorReport *report) {
    CupError err;
    size_t tmp_entries;

    if (report == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = count_tmp_entries(&tmp_entries);
    if (err != CUP_OK) {
        report->warnings++;
        fprintf(stderr, "Warning: could not inspect temporary directory.\n");
        return CUP_OK;
    }

    if (tmp_entries > 0) {
        report->warnings++;
        fprintf(stderr, "Warning: temporary directory contains %zu leftover item(s). Run 'cup repair' to clean them.\n", tmp_entries);
    }

    return CUP_OK;
}

// REPAIR HELPERS
static CupError repair_state_entries(CupState *state, int *changed, int *removed_entries) {
    EntryContext ctx;
    CupError err;
    int on_disk;
    size_t i;

    if (state == NULL || changed == NULL || removed_entries == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    i = 0;
    while (i < state->installed_count) {
        err = prepare_entry_context(state->installed[i].component, state->installed[i].entry, &ctx);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }

        err = build_entry(ctx.canonical_entry, sizeof(ctx.canonical_entry), ctx.tool, ctx.release);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }

        err = install_dir_exists(state->installed[i].component, ctx.tool, state->installed[i].host_platform, 
            state->installed[i].target_platform, ctx.release, &on_disk);
        if (err != CUP_OK) {
            return err;
        }

        if (!on_disk) {
            printf("Removing stale state entry: %s:%s for host '%s', target '%s'.\n",
                state->installed[i].component, state->installed[i].entry,
                state->installed[i].host_platform, state->installed[i].target_platform);

            err = state_remove_default_if_matches(state, state->installed[i].component, state->installed[i].host_platform,
                state->installed[i].target_platform, state->installed[i].entry);
            if (err != CUP_OK) {
                return err;
            }

            err = state_remove_installed(state, state->installed[i].component, state->installed[i].host_platform,
                state->installed[i].target_platform, state->installed[i].entry);
            if (err != CUP_OK) {
                return err;
            }

            *changed = 1;
            (*removed_entries)++;
            continue;
        }

        i++;
    }

    return CUP_OK;
}

static CupError repair_state_defaults(CupState *state, int *changed, int *removed_defaults) {
    CupError err;
    int index;
    size_t i;

    if (state == NULL || changed == NULL || removed_defaults == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    i = 0;
    while (i < state->default_count) {
        index = state_find_installed(state, state->defaults[i].component, state->defaults[i].host_platform,
                state->defaults[i].target_platform, state->defaults[i].entry);
        if (index == -1) {
            printf("Removing stale default: %s:%s for host '%s', target '%s'.\n",
                state->defaults[i].component, state->defaults[i].entry,
                state->defaults[i].host_platform, state->defaults[i].target_platform);

            err = state_remove_default_for_component(state, state->defaults[i].component,
                state->defaults[i].host_platform, state->defaults[i].target_platform);
            if (err != CUP_OK) {
                return err;
            }

            *changed = 1;
            (*removed_defaults)++;
            continue;
        }

        i++;
    }

    return CUP_OK;
}

// INFO PRINTING HELPERS
static void print_info_field(const PackageInfo *info, const char *key, const char *label) {
    const char *value;

    if (info == NULL || is_empty_string(key) || is_empty_string(label)) {
        return;
    }

    value = get_info_value(info, key);
    if (value == NULL) {
        return;
    }

    printf("  %-18s %s\n", label, value);
}

static void print_info_groups(const PackageInfo *info, const char *title, const char *prefix) {
    const PackageInfoField *field;
    size_t cursor;
    int printed_title;

    if (info == NULL || is_empty_string(title) || is_empty_string(prefix)) {
        return;
    }

    cursor = 0;
    printed_title = 0;

    while ((field = next_info_field(info, prefix, &cursor)) != NULL) {
        const char *display_key;

        if (!printed_title) {
            printf("\n%s:\n", title);
            printed_title = 1;
        }

        display_key = field->key + strlen(prefix);
        printf("  %-18s %s\n", display_key, field->value);
    }
}

static void print_package_info(const PackageInfo *info) {
    if (info == NULL) {
        return;
    }

    printf("Package:\n");
    print_info_field(info, "package.component", "component");
    print_info_field(info, "package.tool", "tool");
    print_info_field(info, "package.version", "version");
    print_info_field(info, "platform.host", "host");
    print_info_field(info, "platform.target", "target");

    print_info_groups(info, "Entries", "entry.");
    print_info_groups(info, "Features", "features.");
    print_info_groups(info, "Contents", "contents.");
    print_info_groups(info, "Build/config", "config.");
}

// COMMAND HANDLERS
CupError handle_list(const char *target_override) {
    CommandContext command;
    EntryContext ctx;
    CupError err;
    int printed = 0;
    int has_annotation;
    int is_default;
    int is_stable;
    int on_disk;
    size_t i;

    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < command.state.installed_count; ++i) {
        const char *default_entry;

        if (strcmp(command.state.installed[i].host_platform, command.host_platform) != 0 ||
            strcmp(command.state.installed[i].target_platform, command.target_platform) != 0) {
            continue;
        }

        if (!printed) {
            printf("Installed components for host '%s', target '%s':\n", command.host_platform, command.target_platform);
            printed = 1;
        }
        
        printf("- %s:%s", command.state.installed[i].component, command.state.installed[i].entry);

        err = prepare_entry_context(command.state.installed[i].component, command.state.installed[i].entry, &ctx);
        if (err != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        err = build_entry(ctx.canonical_entry, sizeof(ctx.canonical_entry), ctx.tool, ctx.release);
        if (err != CUP_OK) {
            printf(" (invalid)\n");
            continue;
        }

        err = install_dir_exists(command.state.installed[i].component, ctx.tool, command.host_platform, command.target_platform, 
            ctx.release, &on_disk);
        if (err != CUP_OK) {
            printf(" (could not inspect)\n");
            continue;
        }

        if (!on_disk) {
            printf(" (missing on disk)\n");
            continue;
        }

        default_entry = state_get_default(&command.state, command.state.installed[i].component, command.host_platform, 
            command.target_platform);
        
        is_default = (default_entry != NULL && strcmp(default_entry, ctx.canonical_entry) == 0);
        is_stable = 0;

        err = is_stable_version(&command.manifest, command.state.installed[i].component, ctx.tool, command.host_platform, 
            command.target_platform, ctx.release, &is_stable);
        if (err != CUP_OK) {
            is_stable = 0;
        }

        has_annotation = 0;

        if (is_default || is_stable) {
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
            }

            printf(")");
        }

        printf("\n");
    }

    if (!printed) {
        printf("No components installed for host '%s', target '%s'.\n", command.host_platform, command.target_platform);
    }

    return CUP_OK;
}

CupError handle_install(const char *component, const char *entry, const char *target_override, const char *format_override) {
    CommandContext command;
    EntryContext ctx;
    CupError err;
    CupError rollback_err;
    InstallTransaction tx;
    char archive_format[MAX_NAME_LEN];
    int version_available;
    int in_state;
    int on_disk;

    memset(&tx, 0, sizeof(tx));
    interrupt_setup();

    printf("==> Validating request...\n");
    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Resolving release...\n");
    err = resolve_entry_release(&command.manifest, component, command.host_platform, command.target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = is_version_available(&command.manifest, component, ctx.tool, command.host_platform, command.target_platform, 
        ctx.resolved_release, &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr, "Error: version '%s' is not available for tool '%s' on host '%s', target '%s'.\n",
            ctx.resolved_release, ctx.tool, command.host_platform, command.target_platform);
        return CUP_ERR_NOT_AVAILABLE;
    }

    printf("==> Resolving package format...\n");
    err = resolve_command_format(&command.manifest, archive_format, sizeof(archive_format), component, command.host_platform, 
        command.target_platform, format_override, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Checking existing installation...\n");
    err = check_install_presence(&command.state, component, command.host_platform, command.target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_not_installed(component, command.host_platform, command.target_platform, &ctx, in_state, on_disk);
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

    err = stage_package_install(&command.manifest, tx.tmp_path, component, ctx.tool, command.host_platform, command.target_platform, 
        ctx.resolved_release, archive_format);
    if (err != CUP_OK) {
        if (err == CUP_ERR_INTERRUPT) {
            fprintf(stderr, "\nCleaning temporary files...\n");
            fflush(stderr);
        }

        cleanup_transaction(&tx);

        if (err == CUP_ERR_INTERRUPT) {
            fprintf(stderr, "Cleanup completed.\n");
            fflush(stderr);
        }

        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Validating installation...\n");
    err = validate_installation_metadata(tx.tmp_path, component, ctx.tool, command.host_platform, command.target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Preparing final installation directories...\n");
    err = ensure_component_base_dirs(component, ctx.tool, command.host_platform, command.target_platform);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(tx.install_path, sizeof(tx.install_path), component, ctx.tool, command.host_platform, 
        command.target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    printf("==> Committing installation...\n");
    err = move_transaction(&tx, MOVE_TMP_TO_INSTALL);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    err = state_add_installed(&command.state, component, command.host_platform, command.target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_transaction(&tx, MOVE_INSTALL_TO_TMP);
        if (rollback_err != CUP_OK) {
            cleanup_transaction(&tx);
            fprintf(stderr, "Error: failed to add install to state and rollback failed for '%s:%s'.\n", 
                component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        cleanup_transaction(&tx);
        fprintf(stderr, "Error: failed to add install to state. Installation rolled back.\n");
        return err;
    }

    printf("==> Saving state...\n");
    err = state_save(&command.state);
    if (err != CUP_OK) {
        rollback_err = move_transaction(&tx, MOVE_INSTALL_TO_TMP);
        if (rollback_err != CUP_OK) {
            cleanup_transaction(&tx);
            fprintf(stderr, "Error: state save failed and rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        cleanup_transaction(&tx);
        fprintf(stderr, "Error: state save failed. Installation rolled back.\n");
        return CUP_ERR_STATE_SAVE;
    }

    clear_transaction_tmp(&tx);

    printf("Installed %s ", component);
    print_resolved_entry(stdout, &ctx);
    printf(" for host '%s', target '%s' successfully.\n", command.host_platform, command.target_platform);
    return CUP_OK;
}

CupError handle_remove(const char *component, const char *entry, const char *target_override) {
    CommandContext command;
    EntryContext ctx;
    CupError err;
    CupError rollback_err;
    InstallTransaction tx;
    int in_state;
    int on_disk;

    memset(&tx, 0, sizeof(tx));
    interrupt_setup();

    printf("==> Validating request...\n");
    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Resolving release...\n");
    err = resolve_entry_release(&command.manifest, component, command.host_platform, command.target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Checking installed package...\n");
    err = check_install_presence(&command.state, component, command.host_platform, command.target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, command.host_platform, command.target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = create_tmp_dir(tx.tmp_path, sizeof(tx.tmp_path), "remove", component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(tx.install_path, sizeof(tx.install_path), component, ctx.tool, command.host_platform, 
        command.target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    err = stop_if_interrupted(tx.tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = system_remove_directory(tx.tmp_path);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    printf("==> Staging removal...\n");
    err = move_transaction(&tx, MOVE_INSTALL_TO_TMP);
    if (err != CUP_OK) {
        cleanup_transaction(&tx);
        return err;
    }

    err = state_remove_installed(&command.state, component, command.host_platform, command.target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_transaction_tmp(&tx);
        return err;
    }

    err = state_remove_default_if_matches(&command.state, component, command.host_platform, command.target_platform, 
        ctx.canonical_entry);
    if (err != CUP_OK) {
        rollback_err = move_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_transaction_tmp(&tx);
        return err;
    }

    printf("==> Saving state...\n");
    err = state_save(&command.state);
    if (err != CUP_OK) {
        rollback_err = move_transaction(&tx, MOVE_TMP_TO_INSTALL);
        if (rollback_err != CUP_OK) {
            fprintf(stderr, "Error: remove rollback failed for '%s:%s'.\n", component, ctx.input_entry);
            return CUP_ERR_ROLLBACK;
        }

        clear_transaction_tmp(&tx);
        fprintf(stderr, "Error: could not save state after remove.\n");
        return err;
    }

    printf("==> Cleaning temporary files...\n");
    cleanup_transaction(&tx);

    printf("Removed %s ", component);
    print_resolved_entry(stdout, &ctx);
    printf(" for host '%s', target '%s' successfully.\n", command.host_platform, command.target_platform);
    return CUP_OK;
}

CupError handle_default(const char *component, const char *entry, const char *target_override) {
    CommandContext command;
    EntryContext ctx;
    CupError err;
    int in_state;
    int on_disk;

    printf("==> Validating request...\n");
    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Resolving release...\n");
    err = resolve_entry_release(&command.manifest, component, command.host_platform, command.target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Checking installed package...\n");
    err = check_install_presence(&command.state, component, command.host_platform, command.target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, command.host_platform, command.target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = state_set_default(&command.state, component, command.host_platform, command.target_platform, ctx.canonical_entry);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not set default for component '%s'.\n", component);
        return err;
    }

    printf("==> Saving state...\n");
    err = state_save(&command.state);
    if (err != CUP_OK) {
        return err;
    }

    printf("Default %s for host '%s', target '%s' set to ", component, command.host_platform, command.target_platform);
    print_resolved_entry(stdout, &ctx);
    printf(".\n");
    return CUP_OK;
}

CupError handle_current(const char *component, const char *target_override) {
    CommandContext command;
    EntryContext ctx;
    CupError err;
    const char *default_entry;
    int is_stable;
    int in_state;
    int on_disk;

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    default_entry = state_get_default(&command.state, component, command.host_platform, command.target_platform);
    if (default_entry == NULL) {
        printf("No default set for component '%s' on host '%s', target '%s'.\n", 
            component, command.host_platform, command.target_platform);
        return CUP_OK;
    }

    err = prepare_entry_context(component, default_entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_entry_release(&command.manifest, component, command.host_platform, command.target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&command.state, component, command.host_platform, command.target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (!in_state || !on_disk) {
        fprintf(stderr, "Error: default for component '%s' on host '%s', target '%s' points to an inconsistent installation '", 
            component, command.host_platform, command.target_platform);
        print_resolved_entry(stderr, &ctx);
        fprintf(stderr, "'.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }

    is_stable = 0;

    err = is_stable_version(&command.manifest, component, ctx.tool, command.host_platform, command.target_platform, ctx.resolved_release, &is_stable);
    if (err != CUP_OK) {
        is_stable = 0;
    }

    printf("Current %s default for host '%s', target '%s': %s", 
        component, command.host_platform, command.target_platform, ctx.canonical_entry);
    if (is_stable) {
        printf(" (stable)");
    }
    printf("\n");
    return CUP_OK;
}

CupError handle_info(const char *component, const char *entry, const char *target_override) {
    CommandContext command;
    EntryContext ctx;
    PackageInfo info;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char info_path[MAX_PATH_LEN];
    int in_state;
    int on_disk;

    err = load_command_context(&command, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_entry_release(&command.manifest, component, command.host_platform, command.target_platform, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = check_install_presence(&command.state, component, command.host_platform, command.target_platform, &ctx, &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = require_consistent_installed(component, command.host_platform, command.target_platform, &ctx, in_state, on_disk);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(install_path, sizeof(install_path), component, ctx.tool, command.host_platform,
        command.target_platform, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(info_path, sizeof(info_path), install_path, "info.txt");
    if (err != CUP_OK) {
        return err;
    }

    err = info_load(&info, info_path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not read package metadata '%s'.\n", info_path);
        return err;
    }

    printf("Package information for %s ", component);
    print_resolved_entry(stdout, &ctx);
    printf(" on host '%s', target '%s':\n\n", command.host_platform, command.target_platform);

    print_package_info(&info);
    return CUP_OK;
}

CupError handle_doctor(void) {
    CommandContext command;
    DoctorReport report;
    CupError err;

    memset(&command, 0, sizeof(command));
    memset(&report, 0, sizeof(report));

    printf("==> Checking cup installation...\n");
    err = doctor_check_structure(&report);
    if (err != CUP_OK) {
        return err;
    }

    err = doctor_check_state(&command.state, &report);
    if (err != CUP_OK) {
        return err;
    }

    err = doctor_check_manifest(&command.manifest, &report);
    if (err != CUP_OK) {
        return err;
    }

    if (report.issues == 0) {
        err = doctor_check_installed_entries(&command, &report);
        if (err != CUP_OK) {
            return err;
        }

        err = doctor_check_manifest_for_entries(&command, &report);
        if (err != CUP_OK) {
            return err;
        }

        err = doctor_check_info_files(&command, &report);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = doctor_check_tmp(&report);
    if (err != CUP_OK) {
        return err;
    }

    if (report.issues == 0 && report.warnings == 0) {
        printf("Doctor found no issues.\n");
        return CUP_OK;
    }

    if (report.issues == 0) {
        printf("Doctor found %d warning(s), but no blocking issues.\n", report.warnings);
        return CUP_OK;
    }

    printf("Doctor found %d issue(s) and %d warning(s).\n", report.issues, report.warnings);
    printf("Run 'cup repair' after reviewing the reported issues.\n");
    return CUP_ERR_INCONSISTENT_STATE;
}

CupError handle_repair(void) {
    CommandContext command;
    CupError err;
    int changed = 0;
    int removed_entries = 0;
    int removed_defaults = 0;

    memset(&command, 0, sizeof(command));

    printf("==> Repairing cup structure...\n");
    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Cleaning temporary files...\n");
    err = cleanup_all_tmp();
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not clean temporary files.\n");
        return err;
    }

    printf("==> Loading state...\n");
    err = state_load(&command.state);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: state file is invalid and cannot be repaired automatically yet.\n");
        return err;
    }

    printf("==> Repairing state entries...\n");
    err = repair_state_entries(&command.state, &changed, &removed_entries);
    if (err != CUP_OK) {
        return err;
    }

    err = repair_state_defaults(&command.state, &changed, &removed_defaults);
    if (err != CUP_OK) {
        return err;
    }

    if (changed) {
        printf("==> Saving repaired state...\n");
        err = state_save(&command.state);
        if (err != CUP_OK) {
            return err;
        }
    }

    printf("Repair completed.\n");
    printf("Removed %d stale installed entr%s.\n", removed_entries, removed_entries == 1 ? "y" : "ies");
    printf("Removed %d stale default%s.\n", removed_defaults, removed_defaults == 1 ? "" : "s");

    return CUP_OK;
}

CupError handle_uninstall(void) {
    CupError err;
    char cup_root[MAX_PATH_LEN];
    char uninstall_script[MAX_PATH_LEN];
    char answer[16];
    int exists;

    err = get_cup_root_path(cup_root, sizeof(cup_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_uninstall_script_path(uninstall_script, sizeof(uninstall_script));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_regular_file(uninstall_script, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (!exists) {
        fprintf(stderr, "Error: uninstall script not found at '%s'.\n", uninstall_script);
        fprintf(stderr, "Reinstall cup or remove '%s' manually.\n", cup_root);
        return CUP_ERR_FILESYSTEM;
    }

    printf("This will remove cup and all cup-managed data from:\n");
    printf("  %s\n\n", cup_root);
    printf("The PATH entry will not be removed.\n");
    printf("This is safe to leave in place and will be reused if cup is installed again.\n\n");
    printf("Continue? [y/N] ");

    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        printf("Uninstall cancelled.\n");
        return CUP_OK;
    }

    if (answer[0] != 'y' && answer[0] != 'Y') {
        printf("Uninstall cancelled.\n");
        return CUP_OK;
    }

    err = system_start_uninstall(cup_root, uninstall_script);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not start uninstall process.\n");
        return err;
    }

    printf("Uninstall started.\n");
    printf("cup will remove '%s' shortly and then exit.\n", cup_root);
    printf("The PATH entry was not removed. This is safe to leave in place.\n");
    printf("Open a new terminal if your shell still finds the old cup command.\n");
    return CUP_OK;
}