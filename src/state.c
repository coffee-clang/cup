#include "state.h"
#include "filesystem.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

static void trim_newline(char *s) {
    size_t len;

    if (s == NULL) {
        return;
    }

    len = strlen(s);

    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

static CupError split_state_line(char *line, const char *prefix, char *component, size_t component_size, char *platform, size_t platform_size, const char **entry,  int *matched) {
    char *separator;
    char *key_body;
    char *dot;
    size_t prefix_len;

    if (line == NULL || prefix == NULL || component == NULL || platform == NULL ||
        component_size == 0 || platform_size == 0 || entry == NULL || matched == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *matched = 0;
    *entry = NULL;

    prefix_len = strlen(prefix);

    if (strncmp(line, prefix, prefix_len) != 0) {
        return CUP_OK;
    }

    *matched = 1;

    separator = strchr(line, '=');
    if (separator == NULL) {
        return CUP_ERR_STATE_LOAD;
    }

    *separator = '\0';
    *entry = separator + 1;

    if ((*entry)[0] == '\0') {
        return CUP_ERR_STATE_LOAD;
    }

    key_body = line + prefix_len;
    dot = strchr(key_body, '.');
    if (dot == NULL) {
        return CUP_ERR_STATE_LOAD;
    }

    *dot = '\0';

    if (key_body[0] == '\0' || dot[1] == '\0') {
        return CUP_ERR_STATE_LOAD;
    }

    strncpy(component, key_body, component_size - 1);
    component[component_size - 1] = '\0';

    strncpy(platform, dot + 1, platform_size - 1);
    platform[platform_size - 1] = '\0';

    return CUP_OK;
}

static CupError parse_state_line(CupState *state, char *line) {
    CupError err;
    const char *entry;
    char component[MAX_NAME_LEN];
    char platform[MAX_PLATFORM_LEN];
    int matched;

    if (state == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    trim_newline(line);

    err = split_state_line(line, "installed.", component, sizeof(component), platform, sizeof(platform), &entry, &matched);
    if (err != CUP_OK) {
        return err;
    }

    if (matched) {
        err = state_add_installed(state, component, platform, entry);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }

        return CUP_OK;
    }

    err = split_state_line(line, "default.", component, sizeof(component), platform, sizeof(platform), &entry, &matched);
    if (err != CUP_OK) {
        return err;
    }

    if (matched) {
        err = state_set_default(state, component, platform, entry);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }

        return CUP_OK;
    }

    return CUP_OK;
}

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

    if (state == NULL || filename == NULL || filename[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
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

    return CUP_OK;
}

CupError state_save(const CupState *state, const char *filename) {
    CupError err;
    FILE *file;
    char tmp_filename[MAX_PATH_LEN];
    int status;
    size_t i;

    if (state == NULL || filename == NULL || filename[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    err = checked_snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", filename);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    file = fopen(tmp_filename, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open temporary state file for writing.\n");
        return CUP_ERR_STATE_SAVE;
    }

    for (i = 0; i < state->installed_count; ++i) {
        status = fprintf(file, "installed.%s.%s=%s\n", state->installed[i].component, state->installed[i].platform, state->installed[i].entry);
        if (status < 0) {
            fclose(file);
            remove(tmp_filename);
            fprintf(stderr, "Error: could not write installed state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    for (i = 0; i < state->default_count; ++i) {
        status = fprintf(file, "default.%s.%s=%s\n", state->defaults[i].component, state->defaults[i].platform, state->defaults[i].entry);
        if (status < 0) {
            fclose(file);
            remove(tmp_filename);
            fprintf(stderr, "Error: could not write default state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    status = fclose(file);
    if (status != 0) {
        remove(tmp_filename);
        fprintf(stderr, "Error: could not close temporary state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    status = rename(tmp_filename, filename);
    if (status != 0) {
        remove(tmp_filename);
        fprintf(stderr, "Error: could not replace state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    return CUP_OK;
}

int state_find_installed(const CupState *state, const char *component, const char *platform, const char *entry) {
    size_t i;

    if (state != NULL && component != NULL && platform != NULL && entry != NULL) {
        for (i = 0; i < state->installed_count; ++i) {
            if (strcmp(state->installed[i].component, component) == 0 &&
                strcmp(state->installed[i].platform, platform) == 0 &&
                strcmp(state->installed[i].entry, entry) == 0) {
                return (int)i;
            }
        }
    }

    return -1;
}

CupError state_add_installed(CupState *state, const char *component, const char *platform, const char *entry) {
    int index;

    if (state == NULL || component == NULL || platform == NULL || entry == NULL || 
        component[0] == '\0' || platform[0] == '\0' || entry[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, component, platform, entry);
    if (index != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    strncpy(state->installed[state->installed_count].component, component, MAX_NAME_LEN - 1);
    state->installed[state->installed_count].component[MAX_NAME_LEN - 1] = '\0';
    strncpy(state->installed[state->installed_count].platform, platform, MAX_PLATFORM_LEN - 1);
    state->installed[state->installed_count].platform[MAX_PLATFORM_LEN - 1] = '\0';
    strncpy(state->installed[state->installed_count].entry, entry, MAX_ENTRY_LEN - 1);
    state->installed[state->installed_count].entry[MAX_ENTRY_LEN - 1] = '\0';

    state->installed_count++;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const char *component, const char *platform, const char *entry) {
    int index;
    size_t i;

    if (state == NULL || component == NULL || platform == NULL || entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, component, platform, entry);
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

int state_find_default(const CupState *state, const char *component, const char *platform) {
    size_t i;

    if (state != NULL && component != NULL && platform != NULL) {
        for (i = 0; i < state->default_count; ++i) {
            if (strcmp(state->defaults[i].component, component) == 0 &&
                strcmp(state->defaults[i].platform, platform) == 0) {
                return (int)i;
            }
        }
    }

    return -1;
}

CupError state_set_default(CupState *state, const char *component, const char *platform, const char *entry) {
    int index;

    if (state == NULL || component == NULL || platform == NULL || entry == NULL ||
        component[0] == '\0' || platform[0] == '\0' || entry[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, platform);
    if (index != -1) {
        strncpy(state->defaults[index].entry, entry, MAX_ENTRY_LEN - 1);
        state->defaults[index].entry[MAX_ENTRY_LEN - 1] = '\0';
        return CUP_OK;
    }

    if (state->default_count >= MAX_DEFAULTS) {
        return CUP_ERR_DEFAULT_FULL;
    }

    strncpy(state->defaults[state->default_count].component, component, MAX_NAME_LEN - 1);
    state->defaults[state->default_count].component[MAX_NAME_LEN - 1] = '\0';
    strncpy(state->defaults[state->default_count].platform, platform, MAX_PLATFORM_LEN - 1);
    state->defaults[state->default_count].platform[MAX_PLATFORM_LEN - 1] = '\0';
    strncpy(state->defaults[state->default_count].entry, entry, MAX_ENTRY_LEN - 1);
    state->defaults[state->default_count].entry[MAX_ENTRY_LEN - 1] = '\0';

    state->default_count++;
    return CUP_OK;
}

const char *state_get_default(const CupState *state, const char *component, const char *platform) {
    int index;

    if (state == NULL || component == NULL || platform == NULL ||
        component[0] == '\0' || platform[0] == '\0') {
        return NULL;
    }

    index = state_find_default(state, component, platform);
    if (index == -1) {
        return NULL;
    }

    return state->defaults[index].entry;
}

CupError state_remove_default_for_component(CupState *state, const char *component, const char *platform) {
    int index;
    size_t i;

    if (state == NULL || component == NULL || platform == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, platform);
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

CupError state_remove_default_if_matches(CupState *state, const char *component, const char *platform, const char *entry) {
    CupError err;
    int index;

    if (state == NULL || component == NULL || platform == NULL || entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, platform);
    if (index == -1) {
        return CUP_OK;
    }

    if (strcmp(state->defaults[index].entry, entry) != 0) {
        return CUP_OK;
    }

    err = state_remove_default_for_component(state, component, platform);
    return err;
}