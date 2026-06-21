#include "state.h"

#include "layout.h"
#include "package.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// KEY PARSING
typedef enum {
    STATE_RECORD_UNKNOWN,
    STATE_RECORD_INSTALLED,
    STATE_RECORD_DEFAULT
} StateRecordType;

static CupError parse_state_key(const char *key, StateRecordType *type,
    char *component, size_t component_size,
    char *host_platform, size_t host_size,
    char *target_platform, size_t target_size) {
    static const char installed_prefix[] = "installed.";
    static const char default_prefix[] = "default.";
    TextBuffer parts[3];
    char body[MAX_STATE_LINE_LEN];
    const char *scope;

    if (text_is_empty(key) || type == NULL || component == NULL ||
        component_size == 0 || host_platform == NULL || host_size == 0 ||
        target_platform == NULL || target_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strncmp(key, installed_prefix, sizeof(installed_prefix) - 1) == 0) {
        *type = STATE_RECORD_INSTALLED;
        scope = key + sizeof(installed_prefix) - 1;
    } else if (strncmp(key, default_prefix, sizeof(default_prefix) - 1) == 0) {
        *type = STATE_RECORD_DEFAULT;
        scope = key + sizeof(default_prefix) - 1;
    } else {
        *type = STATE_RECORD_UNKNOWN;
        return CUP_OK;
    }

    if (text_format(body, sizeof(body), "%s", scope) != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    parts[0] = (TextBuffer){.data = component, .capacity = component_size};
    parts[1] = (TextBuffer){.data = host_platform, .capacity = host_size};
    parts[2] = (TextBuffer){.data = target_platform, .capacity = target_size};

    return text_split_exact(body, '.', parts, 3) == CUP_OK
        ? CUP_OK : CUP_ERR_STATE_LOAD;
}

// ENTRY HELPERS
static int state_entry_matches_scope(const StateEntry *state_entry, const char *component,
    const char *host_platform, const char *target_platform) {
    return strcmp(state_entry->component, component) == 0 &&
        strcmp(state_entry->host_platform, host_platform) == 0 &&
        strcmp(state_entry->target_platform, target_platform) == 0;
}

static CupError set_state_entry(StateEntry *state_entry, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;

    if (state_entry == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_format(state_entry->component, sizeof(state_entry->component), "%s", component);
    if (err != CUP_OK) {
        return err;
    }

    err = text_format(state_entry->host_platform,
        sizeof(state_entry->host_platform), "%s", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = text_format(state_entry->target_platform,
        sizeof(state_entry->target_platform), "%s", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    return text_format(state_entry->entry, sizeof(state_entry->entry), "%s", entry);
}

CupError state_validate(const CupState *state) {
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < state->default_count; ++i) {
        const StateEntry *default_entry = &state->defaults[i];

        if (state_find_installed(state, default_entry->component,
            default_entry->host_platform, default_entry->target_platform,
            default_entry->entry) == -1) {
            fprintf(stderr,
                "Error: default state entry '%s' for component '%s', "
                "host '%s', target '%s' is not installed.\n",
                default_entry->entry, default_entry->component,
                default_entry->host_platform, default_entry->target_platform);
            return CUP_ERR_STATE_LOAD;
        }
    }

    return CUP_OK;
}

// LINE PARSING
static CupError parse_state_line(CupState *state, char *line) {
    PackageIdentity identity;
    StateRecordType type;
    CupError err;
    char key[MAX_STATE_LINE_LEN];
    char entry[MAX_ENTRY_LEN];
    char component[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];

    if (state == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_parse_key_value(line, key, sizeof(key), entry, sizeof(entry));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = parse_state_key(key, &type,
        component, sizeof(component),
        host_platform, sizeof(host_platform),
        target_platform, sizeof(target_platform));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: malformed %s state key '%s'.\n",
            type == STATE_RECORD_DEFAULT ? "default" : "installed", key);
        return CUP_ERR_STATE_LOAD;
    }
    if (type == STATE_RECORD_UNKNOWN) {
        fprintf(stderr, "Error: unknown state key '%s'.\n", key);
        return CUP_ERR_STATE_LOAD;
    }

    err = package_identity_from_entry(&identity, component,
        host_platform, target_platform, entry);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    if (type == STATE_RECORD_INSTALLED) {
        err = state_add_installed(state, component,
            host_platform, target_platform, entry);
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            fprintf(stderr,
                "Error: duplicate installed state entry '%s' for "
                "component '%s', host '%s', target '%s'.\n",
                entry, component, host_platform, target_platform);
        }

        return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
    }

    if (state_find_default(state, component, host_platform, target_platform) != -1) {
        fprintf(stderr,
            "Error: duplicate default for component '%s', host '%s', target '%s'.\n",
            component, host_platform, target_platform);
        return CUP_ERR_STATE_LOAD;
    }

    err = state_set_default(state, component,
        host_platform, target_platform, entry);
    return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

// PERSISTENCE
CupError state_load(CupState *state, StateFileStatus *status) {
    CupError err;
    FILE *file;
    char state_path[MAX_PATH_LEN];
    char line[MAX_STATE_LINE_LEN];
    size_t line_number;

    if (state == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(state, 0, sizeof(*state));
    *status = STATE_FILE_MISSING;

    err = layout_get_state_path(state_path, sizeof(state_path));
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

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
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

    *status = STATE_FILE_LOADED;
    return CUP_OK;
}

CupError state_save(const CupState *state) {
    CupError err;
    FILE *file = NULL;
    char root[MAX_PATH_LEN];
    char state_path[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int write_status;
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (state_validate(state) != CUP_OK ||
        layout_get_root(root, sizeof(root)) != CUP_OK ||
        layout_get_state_path(state_path, sizeof(state_path)) != CUP_OK ||
        system_create_temp_file(root, "state", tmp_path,
            sizeof(tmp_path), &file) != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    for (i = 0; i < state->installed_count; ++i) {
        const StateEntry *installed = &state->installed[i];

        write_status = fprintf(file, "installed.%s.%s.%s=%s\n",
            installed->component, installed->host_platform,
            installed->target_platform, installed->entry);
        if (write_status < 0) {
            fclose(file);
            system_remove_file(tmp_path);
            fprintf(stderr, "Error: could not write installed state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    for (i = 0; i < state->default_count; ++i) {
        const StateEntry *default_entry = &state->defaults[i];

        write_status = fprintf(file, "default.%s.%s.%s=%s\n",
            default_entry->component, default_entry->host_platform,
            default_entry->target_platform, default_entry->entry);
        if (write_status < 0) {
            fclose(file);
            system_remove_file(tmp_path);
            fprintf(stderr, "Error: could not write default state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    err = system_sync_file(file);
    write_status = fclose(file);
    if (err != CUP_OK || write_status != 0) {
        system_remove_file(tmp_path);
        fprintf(stderr, "Error: could not close temporary state file.\n");
        return CUP_ERR_STATE_SAVE;
    }

    err = system_replace_file(tmp_path, state_path, &commit_state);
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
            system_remove_file(tmp_path);
            fprintf(stderr, "Error: could not replace state file.\n");
            return CUP_ERR_STATE_SAVE;
        }

        fprintf(stderr, "Error: state file was replaced, but its "
            "durability could not be confirmed.\n");
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

// INSTALLED ENTRIES
int state_find_installed(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    size_t i;

    if (state == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return -1;
    }

    for (i = 0; i < state->installed_count; ++i) {
        if (state_entry_matches_scope(&state->installed[i], component,
            host_platform, target_platform) &&
            strcmp(state->installed[i].entry, entry) == 0) {
            return (int)i;
        }
    }

    return -1;
}

CupError state_add_installed(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;

    if (state == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (state_find_installed(state, component, host_platform, target_platform, entry) != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }

    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    err = set_state_entry(&state->installed[state->installed_count],
        component, host_platform, target_platform, entry);
    if (err != CUP_OK) {
        return err;
    }

    state->installed_count++;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    int index;
    size_t i;

    if (state == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, component, host_platform, target_platform, entry);
    if (index == -1) {
        return CUP_ERR_NOT_INSTALLED;
    }

    for (i = (size_t)index; i + 1 < state->installed_count; ++i) {
        state->installed[i] = state->installed[i + 1];
    }

    state->installed_count--;
    memset(&state->installed[state->installed_count], 0,
        sizeof(state->installed[state->installed_count]));
    return CUP_OK;
}

// DEFAULT ENTRIES
int state_find_default(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform) {
    size_t i;

    if (state == NULL || text_is_empty(component) ||
        text_is_empty(host_platform) || text_is_empty(target_platform)) {
        return -1;
    }

    for (i = 0; i < state->default_count; ++i) {
        if (state_entry_matches_scope(&state->defaults[i], component,
            host_platform, target_platform)) {
            return (int)i;
        }
    }

    return -1;
}

const char *state_get_default(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform) {
    int index;

    index = state_find_default(state, component, host_platform, target_platform);
    return index == -1 ? NULL : state->defaults[index].entry;
}

CupError state_set_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    int index;

    if (state == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index != -1) {
        return text_format(state->defaults[index].entry,
            sizeof(state->defaults[index].entry), "%s", entry);
    }

    if (state->default_count >= MAX_DEFAULTS) {
        return CUP_ERR_DEFAULT_FULL;
    }

    err = set_state_entry(&state->defaults[state->default_count],
        component, host_platform, target_platform, entry);
    if (err != CUP_OK) {
        return err;
    }

    state->default_count++;
    return CUP_OK;
}

CupError state_clear_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform) {
    int index;
    size_t i;

    if (state == NULL || text_is_empty(component) ||
        text_is_empty(host_platform) || text_is_empty(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index == -1) {
        return CUP_OK;
    }

    for (i = (size_t)index; i + 1 < state->default_count; ++i) {
        state->defaults[i] = state->defaults[i + 1];
    }

    state->default_count--;
    memset(&state->defaults[state->default_count], 0,
        sizeof(state->defaults[state->default_count]));
    return CUP_OK;
}

CupError state_clear_matching_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    int index;

    if (state == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform) || text_is_empty(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_default(state, component, host_platform, target_platform);
    if (index == -1 || strcmp(state->defaults[index].entry, entry) != 0) {
        return CUP_OK;
    }

    return state_clear_default(state, component, host_platform, target_platform);
}
