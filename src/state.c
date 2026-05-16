#include "state.h"

#include "filesystem.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// PARSING
static CupError split_state_key(const char *key, const char *prefix, char *component, size_t component_size, char *host_platform, size_t host_size, char *target_platform, size_t target_size, int *matched) {
    const char *body;
    const char *first_dot;
    const char *second_dot;
    const char *third_dot;
    size_t prefix_len;
    size_t component_len;
    size_t host_len;
    size_t target_len;

    if (component == NULL || host_platform == NULL || target_platform == NULL || matched == NULL ||
        component_size == 0 || host_size == 0 || target_size == 0 || is_empty_string(key) || is_empty_string(prefix)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *matched = 0;

    prefix_len = strlen(prefix);
    if (strncmp(key, prefix, prefix_len) != 0) {
        return CUP_OK;
    }

    *matched = 1;
    body = key + prefix_len;

    first_dot = strchr(body, '.');
    if (first_dot == NULL) {
        return CUP_ERR_STATE_LOAD;
    }

    second_dot = strchr(first_dot + 1, '.');
    if (second_dot == NULL) {
        return CUP_ERR_STATE_LOAD;
    }

    third_dot = strchr(second_dot + 1, '.');
    if (third_dot != NULL) {
        return CUP_ERR_STATE_LOAD;
    }

    component_len = (size_t)(first_dot - body);
    host_len = (size_t)(second_dot - first_dot - 1);
    target_len = strlen(second_dot + 1);

    if (component_len == 0 || host_len == 0 || target_len == 0 ||
        component_len >= component_size ||
        host_len >= host_size ||
        target_len >= target_size) {
        return CUP_ERR_STATE_LOAD;
    }

    memcpy(component, body, component_len);
    component[component_len] = '\0';

    memcpy(host_platform, first_dot + 1, host_len);
    host_platform[host_len] = '\0';

    memcpy(target_platform, second_dot + 1, target_len);
    target_platform[target_len] = '\0';

    return CUP_OK;
}

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
    return err;
}

static CupError validate_state_line(char *line) {
    char *trimmed;

    if (line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    trim_line_end(line);
    trimmed = trim_spaces(line);

    if (trimmed[0] == '\0' || trimmed[0] == '#') {
        return CUP_OK;
    }

    if (strchr(trimmed, '=') == NULL) {
        fprintf(stderr, "Error: malformed state line '%s'.\n", trimmed);
        return CUP_ERR_STATE_LOAD;
    }

    return CUP_OK;
}

static CupError validate_state_entry_value(const char *component, const char *entry) {
    CupError err;
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];

    if (is_empty_string(component) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_once(entry, '@', tool, sizeof(tool), release, sizeof(release));
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

static CupError validate_state_key_parts(const char *component, const char *host_platform, const char *target_platform) {
    CupError err;

    if (is_empty_string(component) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
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

    return CUP_OK;
}

static CupError validate_state_defaults(const CupState *state) {
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

static CupError parse_state_line(CupState *state, char *line) {
    CupError err;
    char key[MAX_STATE_LINE_LEN];
    char value[MAX_ENTRY_LEN];
    char component[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char *line_key;
    char *line_value;
    int has_pair;
    int is_installed;
    int is_default;
    int index;

    if (state == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_state_line(line);
    if (err != CUP_OK) {
        return err;
    }

    err = parse_key_value_line(line, &line_key, &line_value, &has_pair);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    if (!has_pair) {
        return CUP_OK;
    }

    err = checked_snprintf(key, sizeof(key), "%s", line_key);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = checked_snprintf(value, sizeof(value), "%s", line_value);
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

    err = validate_state_key_parts(component, host_platform, target_platform);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_state_entry_value(component, value);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    if (is_installed) {
        index = state_find_installed(state, component, host_platform, target_platform, value);
        if (index != -1) {
            fprintf(stderr, "Error: duplicate installed state entry '%s' for component '%s', host '%s', target '%s'.\n",
                value, component, host_platform, target_platform);
            return CUP_ERR_STATE_LOAD;
        }

        err = state_add_installed(state, component, host_platform, target_platform, value);
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
CupError state_init(CupState *state) {
    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(state, 0, sizeof(*state));
    return CUP_OK;
}

CupError state_load(CupState *state, const char *filename) {
    CupError err;
    FILE *file;
    char line[MAX_STATE_LINE_LEN];

    if (state == NULL || is_empty_string(filename)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = state_init(state);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    file = fopen(filename, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not open state file for reading.\n");
        return CUP_ERR_STATE_LOAD;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        err = parse_state_line(state, line);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_STATE_LOAD;
        }
    }

    if (fclose(file) != 0) {
        return CUP_ERR_STATE_LOAD;
    }

    err = validate_state_defaults(state);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError state_save(const CupState *state, const char *filename) {
    CupError err;
    FILE *file;
    char tmp_filename[MAX_PATH_LEN];
    char suffix[MAX_NAME_LEN];
    int status;
    size_t i;

    if (state == NULL || is_empty_string(filename)) {
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

    err = checked_snprintf(tmp_filename, sizeof(tmp_filename), "%s.%s.tmp", filename, suffix);
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

    err = system_rename_path(tmp_filename, filename);
    if (err != CUP_OK) {
        system_remove_file(tmp_filename);
        fprintf(stderr, "Error: could not replace state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    return CUP_OK;
}

// INSTALLED ENTRIES
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

// DEFAULT ENTRIES
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