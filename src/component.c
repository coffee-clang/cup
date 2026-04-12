#include <stdio.h>
#include <string.h>

#include "component.h"
#include "state.h"
#include "fs.h"

static int is_valid_component(const char *component) {
    return strcmp(component, "compiler") == 0;
}

static int is_valid_entry(const char *entry) {
    const char *at = strchr(entry, '@');

    if (!at) 
        return 0;

    if (at == entry) 
        return 0;

    if (*(at + 1) == '\0') 
        return 0;

    if (strchr(at + 1, '@') != NULL) 
        return 0;

    return 1;
}

static int split_entry(const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *at = strchr(entry, '@');
    size_t tool_len;
    size_t release_len;

    if (!at) 
        return 1;

    tool_len = (size_t)(at - entry);
    release_len = strlen(at + 1);

    if (tool_len == 0 || release_len == 0) 
        return 1;

    if (tool_len >= tool_size || release_len >= release_size) 
        return 1;

    strncpy(tool, entry, tool_len);
    tool[tool_len] = '\0';

    strncpy(release, at + 1, release_len);
    release[release_len] = '\0';

    return 0;
}

int handle_list(void) {
    CupState state;
    char state_file[MAX_PATH_LEN];
    int i;

    if (get_state_file_path(state_file, sizeof(state_file)) != 0) 
        return 1;

    if (state_load(&state, state_file) != 0) 
        return 1;

    if (state.installed_count == 0) {
        printf("No components installed yet.\n");
        return 0;
    }

    printf("Installed components:\n");

    for (i = 0; i < state.installed_count; ++i) {
        const char *default_entry;

        printf("- %s:%s", state.installed[i].component, state.installed[i].entry);

        default_entry = state_get_default(&state, state.installed[i].component);
        if (default_entry != NULL && strcmp(default_entry, state.installed[i].entry) == 0)
            printf(" (default)");

        printf("\n");
    }

    return 0;
}

int handle_install(const char *component, const char *entry) {
    CupState state;
    char state_file[MAX_PATH_LEN];
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    char tmp_path[MAX_PATH_LEN];
    char final_path[MAX_PATH_LEN];
    int result;

    if (!is_valid_component(component)) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return 1;
    }

    if (!is_valid_entry(entry)) {
        fprintf(stderr, "Error: invalid entry format.\n");
        return 1;
    }

    if (split_entry(entry, tool, sizeof(tool), release, sizeof(release)) != 0) 
        return 1;

    if (get_state_file_path(state_file, sizeof(state_file)) != 0) 
        return 1;

    if (state_load(&state, state_file) != 0) 
        return 1;

    result = state_add_installed(&state, component, entry);
    if (result == 1) 
        return 1;

    if (result == 2) {
        fprintf(stderr, "Error: already installed.\n");
        return 1;
    }

    if (create_tmp_install_dir(tmp_path, sizeof(tmp_path), component, tool, release) != 0) 
        return 1;

    if (simulate_install(tmp_path, component, tool, release) != 0) {
        cleanup_tmp_install(tmp_path);
        return 1;
    }

    if (validate_install(tmp_path) != 0) {
        cleanup_tmp_install(tmp_path);
        return 1;
    }

    if (build_install_path(final_path, sizeof(final_path), component, tool, release) != 0) {
        cleanup_tmp_install(tmp_path);
        return 1;
    }

    if (ensure_component_dirs(component, tool, get_platform_name()) != 0) {
        cleanup_tmp_install(tmp_path);
        return 1;
    }

    if (commit_install(tmp_path, final_path) != 0) {
        cleanup_tmp_install(tmp_path);
        return 1;
    }

    if (state_save(&state, state_file) != 0) {
        fprintf(stderr, "Warning: install done but state not saved.\n");
        return 1;
    }

    printf("Installed %s %s successfully.\n", component, entry);
    return 0;
}

int handle_remove(const char *component, const char *entry) {
    CupState state;
    char state_file[MAX_PATH_LEN];
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    int result;

    if (!is_valid_component(component)) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return 1;
    }

    if (!is_valid_entry(entry)) {
        fprintf(stderr, "Error: invalid entry format. Use <tool>@<release>.\n");
        return 1;
    }

    if (split_entry(entry, tool, sizeof(tool), release, sizeof(release)) != 0) {
        fprintf(stderr, "Error: invalid entry '%s'.\n", entry);
        return 1;
    }

    if (get_state_file_path(state_file, sizeof(state_file)) != 0)
        return 1;

    if (state_load(&state, state_file) != 0)
        return 1;

    result = state_remove_installed(&state, component, entry);
    if (result != 0) {
        fprintf(stderr, "Error: '%s:%s' is not installed.\n", component, entry);
        return 1;
    }

    state_remove_default(&state, component, entry);

    if (remove_component_install_dir(component, tool, release) != 0)
        return 1;

    if (state_save(&state, state_file) != 0)
        return 1;

    printf("Removed %s %s successfully.\n", component, entry);
    return 0;
}

int handle_default(const char *component, const char *entry) {
    CupState state;
    char state_file[MAX_PATH_LEN];
    int result;

    if (!is_valid_component(component)) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return 1;
    }

    if (!is_valid_entry(entry)) {
        fprintf(stderr, "Error: invalid entry format. Use <tool>@<release>.\n");
        return 1;
    }

    if (get_state_file_path(state_file, sizeof(state_file)) != 0)
        return 1;

    if (state_load(&state, state_file) != 0)
        return 1;

    if (state_find_installed(&state, component, entry) == -1) {
        fprintf(stderr, "Error: '%s:%s' is not installed.\n", component, entry);
        return 1;
    }

    result = state_set_default(&state, component, entry);
    if (result != 0) {
        fprintf(stderr, "Error: could not set default for component '%s'.\n", component);
        return 1;
    }

    if (state_save(&state, state_file) != 0)
        return 1;

    printf("Default %s set to '%s'.\n", component, entry);
    return 0;
}

int handle_current(const char *component) {
    CupState state;
    char state_file[MAX_PATH_LEN];
    const char *default_entry;

    if (!is_valid_component(component)) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return 1;
    }

    if (get_state_file_path(state_file, sizeof(state_file)) != 0)
        return 1;

    if (state_load(&state, state_file) != 0)
        return 1;

    default_entry = state_get_default(&state, component);
    if (default_entry == NULL) {
        printf("No default set for component '%s'.\n", component);
        return 0;
    }

    printf("Current %s default: %s\n", component, default_entry);
    return 0;
}