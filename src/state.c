/*
 * Parses, validates and atomically saves state.txt. Installed identities and defaults are
 * bounded explicitly, and defaults must reference installed identities in the same scope.
 */

#include "state.h"

#include "layout.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key parsing. */
typedef enum {
    STATE_RECORD_UNKNOWN,
    STATE_RECORD_INSTALLED,
    STATE_RECORD_DEFAULT
} StateRecordType;

static CupError parse_state_key(const char *key,
                                StateRecordType *type,
                                char *component,
                                size_t component_size,
                                char *host_platform,
                                size_t host_size,
                                char *target_platform,
                                size_t target_size) {
    static const char installed_prefix[] = "installed.";
    static const char default_prefix[] = "default.";
    TextBuffer parts[3];
    char body[MAX_STATE_LINE_LEN];
    const char *scope;

    if (text_is_empty(key) || type == NULL || component == NULL || component_size == 0 ||
        host_platform == NULL || host_size == 0 || target_platform == NULL || target_size == 0) {
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

    if (text_copy(body, sizeof(body), scope) != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    parts[0] = (TextBuffer){.data = component, .capacity = component_size};
    parts[1] = (TextBuffer){.data = host_platform, .capacity = host_size};
    parts[2] = (TextBuffer){.data = target_platform, .capacity = target_size};

    return text_split_exact(body, '.', parts, 3) == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

/* Identity helpers. */
static int identity_matches_scope(const PackageIdentity *identity, const PackageScope *scope) {
    return identity != NULL && scope != NULL &&
           strcmp(identity->component, scope->component) == 0 &&
           strcmp(identity->host_platform, scope->host_platform) == 0 &&
           strcmp(identity->target_platform, scope->target_platform) == 0;
}

static CupError identity_scope(const PackageIdentity *identity, PackageScope *scope) {
    CupError err;

    if (identity == NULL || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_validate(identity);
    if (err != CUP_OK) {
        return err;
    }

    return package_scope_init(
        scope, identity->component, identity->host_platform, identity->target_platform);
}

/* Complete in-memory model validation. */
CupError state_validate(const CupState *state) {
    size_t i;
    size_t j;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (state->installed_count > MAX_INSTALLED || state->active_count > MAX_ACTIVE_PACKAGES) {
        fprintf(stderr, "Error: state record count exceeds its supported capacity.\n");
        return CUP_ERR_STATE_LOAD;
    }

    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *installed = &state->installed[i];

        if (package_identity_validate(installed) != CUP_OK) {
            fprintf(stderr, "Error: installed state identity %zu is invalid.\n", i + 1);
            return CUP_ERR_STATE_LOAD;
        }

        for (j = 0; j < i; ++j) {
            if (package_identity_equals(&state->installed[j], installed)) {
                fprintf(stderr,
                        "Error: duplicate installed state identity '%s:%s@%s'.\n",
                        installed->component,
                        installed->tool,
                        installed->version);
                return CUP_ERR_STATE_LOAD;
            }
        }
    }

    for (i = 0; i < state->active_count; ++i) {
        const PackageIdentity *active_identity = &state->active[i];
        PackageScope scope;

        if (identity_scope(active_identity, &scope) != CUP_OK) {
            fprintf(stderr, "Error: default state identity %zu is invalid.\n", i + 1);
            return CUP_ERR_STATE_LOAD;
        }

        for (j = 0; j < i; ++j) {
            if (identity_matches_scope(&state->active[j], &scope)) {
                fprintf(stderr,
                        "Error: duplicate default scope for component '%s', "
                        "host '%s', target '%s'.\n",
                        scope.component,
                        scope.host_platform,
                        scope.target_platform);
                return CUP_ERR_STATE_LOAD;
            }
        }

        if (state_find_installed(state, active_identity) == -1) {
            fprintf(stderr,
                    "Error: default state identity '%s@%s' for component '%s', "
                    "host '%s', target '%s' is not installed.\n",
                    active_identity->tool,
                    active_identity->version,
                    active_identity->component,
                    active_identity->host_platform,
                    active_identity->target_platform);
            return CUP_ERR_STATE_LOAD;
        }
    }

    return CUP_OK;
}

size_t state_count_foreign_hosts(const CupState *state, const char *current_host) {
    size_t count = 0;
    size_t i;

    if (state == NULL || text_is_empty(current_host)) {
        return 0;
    }

    for (i = 0; i < state->installed_count; ++i) {
        if (strcmp(state->installed[i].host_platform, current_host) != 0) {
            count++;
        }
    }
    for (i = 0; i < state->active_count; ++i) {
        if (strcmp(state->active[i].host_platform, current_host) != 0) {
            count++;
        }
    }

    return count;
}

CupError state_validate_current_host(const CupState *state, const char *current_host) {
    size_t foreign_count;

    if (state == NULL || text_is_empty(current_host)) {
        return CUP_ERR_INVALID_INPUT;
    }

    foreign_count = state_count_foreign_hosts(state, current_host);
    if (foreign_count == 0) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: state.txt contains %zu record(s) for a foreign host; "
            "run 'cup doctor' before using operational commands.\n",
            foreign_count);
    return CUP_ERR_INCONSISTENT_STATE;
}

/* Strict state.txt parsing. */
static CupError parse_state_line(CupState *state, char *line) {
    PackageIdentity identity;
    PackageScope scope;
    StateRecordType type = STATE_RECORD_UNKNOWN;
    CupError err;
    char key[MAX_STATE_LINE_LEN];
    char selector[MAX_SELECTOR_LEN];
    char component[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];

    if (state == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_parse_key_value(line, key, sizeof(key), selector, sizeof(selector));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = parse_state_key(key,
                          &type,
                          component,
                          sizeof(component),
                          host_platform,
                          sizeof(host_platform),
                          target_platform,
                          sizeof(target_platform));
    if (err != CUP_OK) {
        const char *record_name = "unknown";

        if (type == STATE_RECORD_DEFAULT) {
            record_name = "default";
        } else if (type == STATE_RECORD_INSTALLED) {
            record_name = "installed";
        }
        fprintf(stderr, "Error: malformed %s state key '%s'.\n", record_name, key);
        return CUP_ERR_STATE_LOAD;
    }
    if (type == STATE_RECORD_UNKNOWN) {
        fprintf(stderr, "Error: unknown state key '%s'.\n", key);
        return CUP_ERR_STATE_LOAD;
    }

    err = package_identity_from_selector(
        &identity, component, host_platform, target_platform, selector);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    if (type == STATE_RECORD_INSTALLED) {
        err = state_add_installed(state, &identity);
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            fprintf(stderr,
                    "Error: duplicate installed state record '%s' for "
                    "component '%s', host '%s', target '%s'.\n",
                    selector,
                    component,
                    host_platform,
                    target_platform);
        }

        return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
    }

    err = package_identity_get_scope(&identity, &scope);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }
    if (state_find_active(state, &scope) != -1) {
        fprintf(stderr,
                "Error: duplicate default for component '%s', host '%s', target '%s'.\n",
                component,
                host_platform,
                target_platform);
        return CUP_ERR_STATE_LOAD;
    }

    err = state_set_active(state, &identity);
    return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

/* Persistence. */
CupError state_load(CupState *state, StateFileStatus *status) {
    CupError err;
    FILE *file;
    char state_path[MAX_PATH_LEN];
    char line[MAX_STATE_LINE_LEN];
    size_t line_number = 0;
    int has_line;

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

    err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
    if (err != CUP_OK || !has_line || strcmp(line, "format=1") != 0) {
        fprintf(stderr, "Error: state.txt must start with the supported 'format=1' header.\n");
        fclose(file);
        memset(state, 0, sizeof(*state));
        return CUP_ERR_STATE_LOAD;
    }

    while (1) {
        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: could not read state file line.\n");
            fclose(file);
            memset(state, 0, sizeof(*state));
            return CUP_ERR_STATE_LOAD;
        }
        if (!has_line) {
            break;
        }

        err = parse_state_line(state, line);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid state file line %zu.\n", line_number);
            fclose(file);
            memset(state, 0, sizeof(*state));
            return CUP_ERR_STATE_LOAD;
        }
    }

    if (fclose(file) != 0) {
        memset(state, 0, sizeof(*state));
        return CUP_ERR_STATE_LOAD;
    }

    *status = STATE_FILE_LOADED;
    return CUP_OK;
}

static int compare_identity_pointers(const void *left, const void *right) {
    const PackageIdentity *a = *(const PackageIdentity *const *)left;
    const PackageIdentity *b = *(const PackageIdentity *const *)right;
    int result;

    result = strcmp(a->component, b->component);
    if (result == 0)
        result = strcmp(a->host_platform, b->host_platform);
    if (result == 0)
        result = strcmp(a->target_platform, b->target_platform);
    if (result == 0)
        result = strcmp(a->tool, b->tool);
    if (result == 0)
        result = strcmp(a->version, b->version);
    return result;
}

/* Atomic persistence and commit-state handling. */
CupError state_save(const CupState *state) {
    CupError err;
    FILE *file = NULL;
    char root[MAX_PATH_LEN];
    char state_path[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    const PackageIdentity *installed[MAX_INSTALLED];
    const PackageIdentity *active[MAX_ACTIVE_PACKAGES];
    int write_status;
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (state_validate(state) != CUP_OK || layout_get_root(root, sizeof(root)) != CUP_OK ||
        layout_get_state_path(state_path, sizeof(state_path)) != CUP_OK ||
        system_create_temp_file(root, "state", tmp_path, sizeof(tmp_path), &file) != CUP_OK) {
        return CUP_ERR_STATE_SAVE;
    }

    if (fprintf(file, "format=%d\n", CUP_STATE_FORMAT) < 0) {
        fclose(file);
        system_remove_file(tmp_path);
        return CUP_ERR_STATE_SAVE;
    }

    for (i = 0; i < state->installed_count; ++i) {
        installed[i] = &state->installed[i];
    }
    for (i = 0; i < state->active_count; ++i) {
        active[i] = &state->active[i];
    }
    qsort(installed, state->installed_count, sizeof(installed[0]), compare_identity_pointers);
    qsort(active, state->active_count, sizeof(active[0]), compare_identity_pointers);

    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *installed_identity = installed[i];
        char selector[MAX_SELECTOR_LEN];

        if (package_identity_format_selector(installed_identity, selector, sizeof(selector)) !=
            CUP_OK) {
            fclose(file);
            system_remove_file(tmp_path);
            return CUP_ERR_STATE_SAVE;
        }
        write_status = fprintf(file,
                               "installed.%s.%s.%s=%s\n",
                               installed_identity->component,
                               installed_identity->host_platform,
                               installed_identity->target_platform,
                               selector);
        if (write_status < 0) {
            fclose(file);
            system_remove_file(tmp_path);
            fprintf(stderr, "Error: could not write installed state.\n");
            return CUP_ERR_STATE_SAVE;
        }
    }

    for (i = 0; i < state->active_count; ++i) {
        const PackageIdentity *active_identity = active[i];
        char selector[MAX_SELECTOR_LEN];

        if (package_identity_format_selector(active_identity, selector, sizeof(selector)) !=
            CUP_OK) {
            fclose(file);
            system_remove_file(tmp_path);
            return CUP_ERR_STATE_SAVE;
        }
        write_status = fprintf(file,
                               "default.%s.%s.%s=%s\n",
                               active_identity->component,
                               active_identity->host_platform,
                               active_identity->target_platform,
                               selector);
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

        fprintf(stderr,
                "Error: state file was replaced, but its "
                "durability could not be confirmed.\n");
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

/* Installed identities. */
int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    size_t i;

    if (state == NULL || package_identity_validate(identity) != CUP_OK) {
        return -1;
    }

    for (i = 0; i < state->installed_count; ++i) {
        if (package_identity_equals(&state->installed[i], identity)) {
            return (int)i;
        }
    }

    return -1;
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    CupError err;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_validate(identity);
    if (err != CUP_OK) {
        return err;
    }
    if (state_find_installed(state, identity) != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }
    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    state->installed[state->installed_count++] = *identity;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const PackageIdentity *identity) {
    int index;
    size_t i;

    if (state == NULL || package_identity_validate(identity) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_installed(state, identity);
    if (index == -1) {
        return CUP_ERR_NOT_INSTALLED;
    }

    for (i = (size_t)index; i + 1 < state->installed_count; ++i) {
        state->installed[i] = state->installed[i + 1];
    }

    state->installed_count--;
    memset(&state->installed[state->installed_count],
           0,
           sizeof(state->installed[state->installed_count]));
    return CUP_OK;
}

/* Default identities. */
int state_find_active(const CupState *state, const PackageScope *scope) {
    PackageScope validated;
    size_t i;

    if (state == NULL || scope == NULL ||
        package_scope_init(
            &validated, scope->component, scope->host_platform, scope->target_platform) != CUP_OK) {
        return -1;
    }

    for (i = 0; i < state->active_count; ++i) {
        if (identity_matches_scope(&state->active[i], scope)) {
            return (int)i;
        }
    }

    return -1;
}

const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope) {
    int index = state_find_active(state, scope);

    return index == -1 ? NULL : &state->active[index];
}

CupError state_set_active(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    CupError err;
    int index;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = identity_scope(identity, &scope);
    if (err != CUP_OK) {
        return err;
    }

    index = state_find_active(state, &scope);
    if (index != -1) {
        state->active[index] = *identity;
        return CUP_OK;
    }
    if (state->active_count >= MAX_ACTIVE_PACKAGES) {
        return CUP_ERR_ACTIVE_FULL;
    }

    state->active[state->active_count++] = *identity;
    return CUP_OK;
}

CupError state_clear_active(CupState *state, const PackageScope *scope) {
    int index;
    size_t i;

    if (state == NULL || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = state_find_active(state, scope);
    if (index == -1) {
        return CUP_OK;
    }

    for (i = (size_t)index; i + 1 < state->active_count; ++i) {
        state->active[i] = state->active[i + 1];
    }

    state->active_count--;
    memset(&state->active[state->active_count], 0, sizeof(state->active[state->active_count]));
    return CUP_OK;
}

CupError state_clear_matching_active(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    const PackageIdentity *current;
    CupError err;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = identity_scope(identity, &scope);
    if (err != CUP_OK) {
        return err;
    }

    current = state_get_active(state, &scope);
    if (current == NULL || !package_identity_equals(current, identity)) {
        return CUP_OK;
    }

    return state_clear_active(state, &scope);
}
