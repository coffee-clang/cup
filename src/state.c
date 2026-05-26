#include "state.h"

#include "filesystem.h"
#include "platform.h"
#include "registry.h"
#include "entry.h"
#include "system.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// KEY PARSING
static CupError split_state_key(const char *key, const char *prefix, char *component, size_t component_size, char *host_platform, size_t host_size, char *target_platform, size_t target_size, int *matched) {
    CupError err;
    SplitOutput split_outputs[3];
    char body_copy[MAX_STATE_LINE_LEN];
    const char *body;
    size_t prefix_len;

    if (component == NULL || host_platform == NULL || target_platform == NULL || matched == NULL ||
        component_size == 0 || host_size == 0 || target_size == 0 || is_empty_string(key) || is_empty_string(prefix)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *matched = 0;
    prefix_len = strlen(prefix);

    if (strncmp(key, prefix, prefix_len) != 0) {
        return CUP_OK;
    }

    body = key + prefix_len;
    *matched = 1;

    err = checked_snprintf(body_copy, sizeof(body_copy), "%s", body);
    if (err != CUP_OK) {
        return err;
    }

    split_outputs[0].buffer = component;
    split_outputs[0].size = component_size;
    split_outputs[1].buffer = host_platform;
    split_outputs[1].size = host_size;
    split_outputs[2].buffer = target_platform;
    split_outputs[2].size = target_size;

    err = split_exact(body_copy, '.', split_outputs, 3);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// ENTRY STORAGE HELPERS
static CupError set_state_entry(StateEntry *state_entry, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;

    if (state_entry == NULL || is_empty_string(component) || is_empty_string(host_platform) ||
        is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(state_entry->component, sizeof(state_entry->component), "%s", component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(state_entry->host_platform, sizeof(state_entry->host_platform), "%s", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(state_entry->target_platform, sizeof(state_entry->target_platform), "%s", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(state_entry->entry, sizeof(state_entry->entry), "%s", entry);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// LINE/GLOBAL VALIDATION
static CupError validate_state_line(const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];

    if (is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_component(component);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_platform(host_platform);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_platform(target_platform);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = parse_entry(entry, tool, sizeof(tool), release, sizeof(release));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: malformed state entry '%s'. Expected '<tool>@<release>'.\n", entry);
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_tool_for_component(component, tool);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    return CUP_OK;
}

static CupError validate_state(const CupState *state) {
    int index;
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = 0;

    for (i = 0; i < state->default_count; ++i) {
        index = state_find_installed(state, state->defaults[i].component, state->defaults[i].host_platform, 
            state->defaults[i].target_platform, state->defaults[i].entry);
        if (index == -1) {
            fprintf(stderr, "Error: default state entry '%s' for component '%s', host '%s', target '%s' is not installed.\n",
                state->defaults[i].entry, state->defaults[i].component, 
                state->defaults[i].host_platform, state->defaults[i].target_platform);
            return CUP_ERR_STATE_LOAD;
        }
    }

    return CUP_OK;
}

// LINE PARSING
static CupError parse_state_line(CupState *state, char *line) {
    CupError err;
    char key[MAX_STATE_LINE_LEN];
    char value[MAX_ENTRY_LEN];
    char component[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    int is_installed;
    int is_default;
    int index;

    if (state == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_key_value(line, key, sizeof(key), value, sizeof(value));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = split_state_key(key, "installed.", component, sizeof(component), host_platform, 
        sizeof(host_platform), target_platform, sizeof(target_platform), &is_installed);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: malformed installed state key '%s'.\n", key);
        return CUP_ERR_STATE_LOAD;
    }

    is_default = 0;

    if (!is_installed) {
        err = split_state_key(key, "default.", component, sizeof(component), host_platform, 
            sizeof(host_platform), target_platform, sizeof(target_platform), &is_default);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: malformed default state key '%s'.\n", key);
            return CUP_ERR_STATE_LOAD;
        }
    }

    if (!is_installed && !is_default) {
        fprintf(stderr, "Error: unknown state key '%s'.\n", key);
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_state_line(component, host_platform, target_platform, value);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    if (is_installed) {
        err = state_add_installed(state, component, host_platform, target_platform, value);
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            fprintf(stderr, "Error: duplicate installed state entry '%s' for component '%s', host '%s', target '%s'.\n",
                value, component, host_platform, target_platform);
            return CUP_ERR_STATE_LOAD;
        }
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }

        return CUP_OK;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index != -1) {
        fprintf(stderr, "Error: duplicate default for component '%s', host '%s', target '%s'.\n",
            component, host_platform, target_platform);
        return CUP_ERR_STATE_LOAD;
    }

    err = state_set_default(state, component, host_platform, target_platform, value);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    return CUP_OK;
}

// PERSISTENCE
CupError state_load(CupState *state) {
    CupError err;
    FILE *file;
    char state_path[MAX_PATH_LEN];
    char line[MAX_STATE_LINE_LEN];
    size_t line_number;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(state, 0, sizeof(*state));

    err = get_state_file_path(state_path, sizeof(state_path));
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(state_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not open state file for reading.\n");
        return CUP_ERR_STATE_LOAD;
    }

    line_number = 0;

    while (1) {
        int has_line;

        err = read_text_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: could not read state file line.\n");
            fclose(file);
            return CUP_ERR_STATE_LOAD;
        }

        if (!has_line) {
            break;
        }

        err = parse_state_line(state, line);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid state file line %zu.\n", line_number);
            fclose(file);
            return CUP_ERR_STATE_LOAD;
        }
    }

    if (fclose(file) != 0) {
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_state(state);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError state_save(const CupState *state) {
    CupError err;
    FILE *file;
    char state_path[MAX_PATH_LEN];
    char tmp_filename[MAX_PATH_LEN];
    char suffix[MAX_NAME_LEN];
    int status;
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    err = system_get_process_id(suffix, sizeof(suffix));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    err = get_state_file_path(state_path, sizeof(state_path));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    err = checked_snprintf(tmp_filename, sizeof(tmp_filename), "%s.%s.tmp", state_path, suffix);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    file = fopen(tmp_filename, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open temporary state file for writing.\n");
        return CUP_ERR_STATE_SAVE;
    }

    for (i = 0; i < state->installed_count; ++i) {
        status = fprintf(file, "installed.%s.%s.%s=%s\n", 
            state->installed[i].component, state->installed[i].host_platform, 
            state->installed[i].target_platform, state->installed[i].entry);
        if (status < 0) {
            fclose(file);
            system_remove_file(tmp_filename);
            fprintf(stderr, "Error: could not write installed state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    for (i = 0; i < state->default_count; ++i) {
        status = fprintf(file, "default.%s.%s.%s=%s\n", 
            state->defaults[i].component, state->defaults[i].host_platform, 
            state->defaults[i].target_platform, state->defaults[i].entry);
        if (status < 0) {
            fclose(file);
            system_remove_file(tmp_filename);
            fprintf(stderr, "Error: could not write default state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    status = fclose(file);
    if (status != 0) {
        system_remove_file(tmp_filename);
        fprintf(stderr, "Error: could not close temporary state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    err = system_rename_path(tmp_filename, state_path);
    if (err != CUP_OK) {
        system_remove_file(tmp_filename);
        fprintf(stderr, "Error: could not replace state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    return CUP_OK;
}

// INSTALLED API
int state_find_installed(const CupState *state, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    size_t i;

    if (state != NULL && !is_empty_string(component) && !is_empty_string(host_platform) && 
        !is_empty_string(target_platform) && !is_empty_string(entry)) {
        for (i = 0; i < state->installed_count; ++i) {
            if (strcmp(state->installed[i].component, component) == 0 && 
                strcmp(state->installed[i].host_platform, host_platform) == 0 &&
                strcmp(state->installed[i].target_platform, target_platform) == 0 && 
                strcmp(state->installed[i].entry, entry) == 0) {
                return (int)i;
            }
        }
    }

    return -1;
}

CupError state_add_installed(CupState *state, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    int index;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, component, host_platform, target_platform, entry);
    if (index != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    err = set_state_entry(&state->installed[state->installed_count], component, host_platform, target_platform, entry);
    if (err != CUP_OK) {
        return err;
    }

    state->installed_count++;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    int index;
    size_t i;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, component, host_platform, target_platform, entry);
    if (index == -1) {
        return CUP_ERR_NOT_INSTALLED;
    }

    for (i = (size_t)index; i < state->installed_count - 1; ++i) {
        state->installed[i] = state->installed[i + 1];
    }

    state->installed_count--;
    memset(&state->installed[state->installed_count], 0, sizeof(state->installed[state->installed_count]));

    return CUP_OK;
}

// DEFAULT API
int state_find_default(const CupState *state, const char *component, const char *host_platform, const char *target_platform) {
    size_t i;

    if (state != NULL && !is_empty_string(component) && 
        !is_empty_string(host_platform) && !is_empty_string(target_platform)) {
        for (i = 0; i < state->default_count; ++i) {
            if (strcmp(state->defaults[i].component, component) == 0 &&
                strcmp(state->defaults[i].host_platform, host_platform) == 0 &&
                strcmp(state->defaults[i].target_platform, target_platform) == 0) {
                return (int)i;
            }
        }
    }

    return -1;
}

CupError state_set_default(CupState *state, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    int index;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index != -1) {
        err = checked_snprintf(state->defaults[index].entry, sizeof(state->defaults[index].entry), "%s", entry);
        return err;
    }

    if (state->default_count >= MAX_DEFAULTS) {
        return CUP_ERR_DEFAULT_FULL;
    }

    err = set_state_entry(&state->defaults[state->default_count], component, host_platform, target_platform, entry);
    if (err != CUP_OK) {
        return err;
    }

    state->default_count++;
    return CUP_OK;
}

const char *state_get_default(const CupState *state, const char *component, const char *host_platform, const char *target_platform) {
    int index;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return NULL;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index == -1) {
        return NULL;
    }

    return state->defaults[index].entry;
}

// DEFAULT CLEANUP
CupError state_remove_default_for_component(CupState *state, const char *component, const char *host_platform, const char *target_platform) {
    int index;
    size_t i;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index == -1) {
        return CUP_OK;
    }

    for (i = (size_t)index; i < state->default_count - 1; ++i) {
        state->defaults[i] = state->defaults[i + 1];
    }

    state->default_count--;
    memset(&state->defaults[state->default_count], 0, sizeof(state->defaults[state->default_count]));

    return CUP_OK;
}

CupError state_remove_default_if_matches(CupState *state, const char *component, const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    int index;

    if (state == NULL || is_empty_string(component) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index == -1) {
        return CUP_OK;
    }

    if (strcmp(state->defaults[index].entry, entry) != 0) {
        return CUP_OK;
    }

    err = state_remove_default_for_component(state, component, host_platform, target_platform);
    return err;
}