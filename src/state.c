#include "state.h"
#include "fs.h"

#include <stdio.h>
#include <string.h>

static void trim_newline(char *s) {
    size_t len = strlen(s);

    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

void state_init(CupState *state) {
    int i;

    state->installed_count = 0;
    state->default_count = 0;

    for (i = 0; i < MAX_INSTALLED; ++i) {
        state->installed[i].component[0] = '\0';
        state->installed[i].entry[0] = '\0';
    }

    for (i = 0; i < MAX_DEFAULTS; ++i) {
        state->defaults[i].component[0] = '\0';
        state->defaults[i].entry[0] = '\0';
    }
}

CupError state_load(CupState *state, const char *filename) {
    CupError err;
    FILE *file;
    char line[256];

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    state_init(state);

    file = fopen(filename, "r");
    if (file == NULL) {
        return CUP_OK;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *at;

        trim_newline(line);

        if (strncmp(line, "installed.", 10) == 0) {
            char component[MAX_NAME_LEN];
            const char *entry;

            at = strchr(line, '=');
            if (at == NULL) {
                continue;
            }

            *at = '\0';
            entry = at + 1;

            strncpy(component, line + 10, MAX_NAME_LEN - 1);
            component[MAX_NAME_LEN - 1] = '\0';

            state_add_installed(state, component, entry);
            continue;
        }

        if (strncmp(line, "default.", 8) == 0) {
            char component[MAX_NAME_LEN];
            const char *entry;

            at = strchr(line, '=');
            if (at == NULL) {
                continue;
            }

            *at = '\0';
            entry = at + 1;

            strncpy(component, line + 8, MAX_NAME_LEN - 1);
            component[MAX_NAME_LEN - 1] = '\0';

            state_set_default(state, component, entry);
        }
    }

    fclose(file);
    return CUP_OK;
}

CupError state_save(const CupState *state, const char *filename) {
    CupError err;
    FILE *file;
    int i;

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    file = fopen(filename, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open state file for writing.\n");
        return CUP_ERR_STATE_SAVE;
    }

    for (i = 0; i < state->installed_count; ++i) {
        fprintf(file, "installed.%s=%s\n", state->installed[i].component, state->installed[i].entry);
    }

    for (i = 0; i < state->default_count; ++i) {
        fprintf(file, "default.%s=%s\n", state->defaults[i].component, state->defaults[i].entry);
    }

    fclose(file);
    return CUP_OK;
}

int state_find_installed(const CupState *state, const char *component, const char *entry) {
    int i;

    for (i = 0; i < state->installed_count; ++i) {
        if (strcmp(state->installed[i].component, component) == 0 && strcmp(state->installed[i].entry, entry) == 0) {
            return i;
        }
    }

    return -1;
}

CupError state_add_installed(CupState *state, const char *component, const char *entry) {
    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    if (state_find_installed(state, component, entry) != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }

    strncpy(state->installed[state->installed_count].component, component, MAX_NAME_LEN - 1);
    state->installed[state->installed_count].component[MAX_NAME_LEN - 1] = '\0';
    strncpy(state->installed[state->installed_count].entry, entry, MAX_ENTRY_LEN - 1);
    state->installed[state->installed_count].entry[MAX_ENTRY_LEN - 1] = '\0';

    state->installed_count++;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const char *component, const char *entry) {
    int index;
    int i;

    index = state_find_installed(state, component, entry);
    if (index == -1) {
        return CUP_ERR_NOT_INSTALLED;
    }

    for (i = index; i < state->installed_count - 1; ++i) {
        state->installed[i] = state->installed[i + 1];
    }

    state->installed_count--;
    state->installed[state->installed_count].component[0] = '\0';
    state->installed[state->installed_count].entry[0] = '\0';

    return CUP_OK;
}

int state_find_default(const CupState *state, const char *component) {
    int i;

    for (i = 0; i < state->default_count; ++i) {
        if (strcmp(state->defaults[i].component, component) == 0) {
            return i;
        }
    }

    return -1;
}

CupError state_set_default(CupState *state, const char *component, const char *entry) {
    int index;

    index = state_find_default(state, component);
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
    strncpy(state->defaults[state->default_count].entry, entry, MAX_ENTRY_LEN - 1);
    state->defaults[state->default_count].entry[MAX_ENTRY_LEN - 1] = '\0';

    state->default_count++;
    return CUP_OK;
}

const char *state_get_default(const CupState *state, const char *component) {
    int index;

    index = state_find_default(state, component);
    if (index == -1) {
        return NULL;
    }

    return state->defaults[index].entry;
}

void state_remove_default_for_component(CupState *state, const char *component) {
    int index;
    int i;

    index = state_find_default(state, component);
    if (index == -1) {
        return;
    }

    for (i = index; i < state->default_count - 1; ++i) {
        state->defaults[i] = state->defaults[i + 1];
    }

    state->default_count--;
    state->defaults[state->default_count].component[0] = '\0';
    state->defaults[state->default_count].entry[0] = '\0';
}

void state_remove_default_if_matches(CupState *state, const char *component, const char *entry) {
    int index;

    index = state_find_default(state, component);
    if (index == -1) {
        return;
    }

    if (strcmp(state->defaults[index].entry, entry) == 0) {
        state_remove_default_for_component(state, component);
    }
}