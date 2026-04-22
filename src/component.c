#include "component.h"
#include "state.h"
#include "fs.h"
#include "manifest.h"
#include "support.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

typedef struct {
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    char resolved_release[MAX_NAME_LEN];
    char canonical_entry[MAX_ENTRY_LEN];
} EntryContext;

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void print_step(const char *message) {
    printf("==> %s\n", message);
}

static int is_valid_entry(const char *entry) {
    const char *at = strchr(entry, '@');

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

static int is_version_like(const char *release) {
    size_t i;

    if (release == NULL || release[0] == '\0') {
        return 0;
    }

    for (i = 0; release[i] != '\0'; ++i) {
        if (!((release[i] >= '0' && release[i] <= '9') || release[i] == '.')) {
            return 0;
        }
    }

    return 1;
}

static CupError validate_release_name(const char *release) {
    if (release == NULL || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(release, "stable") == 0) {
        return CUP_OK;
    }

    if (is_version_like(release)) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported release '%s'.\n", release);
    return CUP_ERR_INVALID_RELEASE;
}

static CupError build_canonical_entry(char *buffer, size_t size, const char *tool, const char *resolved_release) {
    if (buffer == NULL || tool == NULL || resolved_release == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "%s@%s", tool, resolved_release);
}

static CupError resolve_entry_context(const char *component, const char *entry, EntryContext *ctx) {
    CupError err;

    if (component == NULL || entry == NULL || ctx == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

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

    err = validate_release_name(ctx->release);
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_release(ctx->resolved_release, sizeof(ctx->resolved_release), component, ctx->tool, ctx->release);
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

static CupError check_interrupted(const char *tmp_path) {
    if (g_interrupted) {
        if (tmp_path != NULL && tmp_path[0] != '\0') {
            cleanup_tmp_install(tmp_path);
        }

        fprintf(stderr, "Error: installation interrupted.\n");
        return CUP_ERR_INTERRUPT;
    }

    return CUP_OK;
}

CupError handle_list(void) {
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    int i;

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    if (state.installed_count == 0) {
        printf("No components installed yet.\n");
        return CUP_OK;
    }

    printf("Installed components:\n");

    for (i = 0; i < state.installed_count; ++i) {
        const char *default_entry;

        printf("- %s:%s", state.installed[i].component, state.installed[i].entry);

        default_entry = state_get_default(&state, state.installed[i].component);
        if (default_entry != NULL && strcmp(default_entry, state.installed[i].entry) == 0) {
            printf(" (default)");
        }

        printf("\n");
    }

    return CUP_OK;
}

CupError handle_install(const char *component, const char *entry, const char *format_override) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    char archive_format[MAX_NAME_LEN];
    char tmp_path[MAX_PATH_LEN];
    char final_path[MAX_PATH_LEN];
    int version_available;
    int format_supported;
    int in_state;
    int on_disk;

    tmp_path[0] = '\0';
    g_interrupted = 0;
    signal(SIGINT, handle_sigint);

    print_step("Resolving release...");
    err = resolve_entry_context(component, entry, &ctx);
    if (err != CUP_OK) {
        return err;
    }

    err = is_version_available(component, ctx.tool, ctx.resolved_release, &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr, "Error: version '%s' is not available for tool '%s'.\n", ctx.resolved_release, ctx.tool);
        return CUP_ERR_INVALID_RELEASE;
    }

    if (format_override == NULL) {
        err = get_default_format(archive_format, sizeof(archive_format), component, ctx.tool);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: could not determine default archive format for tool '%s'.\n", ctx.tool);
            return err;
        }
    } else {
        err = is_format_supported(component, ctx.tool, format_override, &format_supported);
        if (err != CUP_OK) {
            return err;
        }

        if (!format_supported) {
            fprintf(stderr, "Error: archive format '%s' is not supported for tool '%s'.\n", format_override, ctx.tool);
            return CUP_ERR_INVALID_INPUT;
        }

        err = checked_snprintf(archive_format, sizeof(archive_format), "%s", format_override);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = get_state_file_path(state_file, sizeof(state_file));
    if (err != CUP_OK) {
        return err;
    }

    err = state_load(&state, state_file);
    if (err != CUP_OK) {
        return err;
    }

    in_state = (state_find_installed(&state, component, ctx.canonical_entry) != -1);

    err = installation_exists(component, ctx.tool, ctx.resolved_release, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (in_state && on_disk) {
        fprintf(stderr, "Error: '%s:%s' is already installed.\n", component, entry);
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (in_state != on_disk) {
        fprintf(stderr, "Error: inconsistent install state detected for '%s:%s'.\n", component, entry);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = create_tmp_install_dir(tmp_path, sizeof(tmp_path), component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = check_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Fetching and installing package...");
    err = perform_install(tmp_path, component, ctx.tool, ctx.resolved_release, archive_format);
    if (err != CUP_OK) {
        cleanup_tmp_install(tmp_path);
        return err;
    }

    err = check_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Validating installation...");
    err = validate_install(tmp_path);
    if (err != CUP_OK) {
        cleanup_tmp_install(tmp_path);
        return err;
    }

    err = check_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Preparing final installation directories...");
    err = ensure_component_base_dirs(component, ctx.tool, get_platform_name());
    if (err != CUP_OK) {
        cleanup_tmp_install(tmp_path);
        return err;
    }

    err = check_interrupted(tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = build_install_path(final_path, sizeof(final_path), component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    print_step("Committing installation...");
    err = commit_install(tmp_path, final_path);
    if (err != CUP_OK) {
        cleanup_tmp_install(tmp_path);
        return err;
    }

    tmp_path[0] = '\0';

    err = state_add_installed(&state, component, ctx.canonical_entry);
    if (err != CUP_OK) {
        if (remove_component_install_dir(component, ctx.tool, ctx.resolved_release) != CUP_OK) {
            fprintf(stderr, "Error: failed to add install to state and rollback failed for '%s:%s'.\n", component, entry);
            return CUP_ERR_ROLLBACK;
        }

        fprintf(stderr, "Error: failed to add install to state. Installation rolled back.\n");
        return err;
    }

    print_step("Saving state...");
    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        state_remove_installed(&state, component, ctx.canonical_entry);

        if (remove_component_install_dir(component, ctx.tool, ctx.resolved_release) != CUP_OK) {
            fprintf(stderr, "Error: state save failed and rollback failed for '%s:%s'.\n", component, entry);
            return CUP_ERR_ROLLBACK;
        }

        fprintf(stderr, "Error: state save failed. Installation rolled back.\n");
        return CUP_ERR_STATE_SAVE;
    }

    printf("Installed %s %s successfully.\n", component, entry);
    return CUP_OK;
}

CupError handle_remove(const char *component, const char *entry) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    int on_disk;

    err = resolve_entry_context(component, entry, &ctx);
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

    err = installation_exists(component, ctx.tool, ctx.resolved_release, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (state_find_installed(&state, component, ctx.canonical_entry) == -1) {
        fprintf(stderr, "Error: '%s:%s' is not installed.\n", component, entry);
        return CUP_ERR_NOT_INSTALLED;
    }

    if (!on_disk) {
        fprintf(stderr, "Error: installation '%s:%s' is present in state but missing on disk.\n", component, entry);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = state_remove_installed(&state, component, ctx.canonical_entry);
    if (err != CUP_OK) {
        return err;
    }

    state_remove_default_if_matches(&state, component, ctx.canonical_entry);

    err = remove_component_install_dir(component, ctx.tool, ctx.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = state_save(&state, state_file);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not save state after remove.\n");
        return err;
    }

    printf("Removed %s %s successfully.\n", component, entry);
    return CUP_OK;
}

CupError handle_default(const char *component, const char *entry) {
    EntryContext ctx;
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];

    err = resolve_entry_context(component, entry, &ctx);
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

    if (state_find_installed(&state, component, ctx.canonical_entry) == -1) {
        fprintf(stderr, "Error: '%s:%s' is not installed.\n", component, entry);
        return CUP_ERR_NOT_INSTALLED;
    }

    err = state_set_default(&state, component, ctx.canonical_entry);
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

CupError handle_current(const char *component) {
    CupState state;
    CupError err;
    char state_file[MAX_PATH_LEN];
    const char *default_entry;

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

    default_entry = state_get_default(&state, component);
    if (default_entry == NULL) {
        printf("No default set for component '%s'.\n", component);
        return CUP_OK;
    }

    printf("Current %s default: %s\n", component, default_entry);
    return CUP_OK;
}