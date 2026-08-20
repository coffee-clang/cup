/*
 * Parses, validates and atomically saves state.txt. Installed identities and defaults are
 * bounded explicitly, and defaults must reference installed identities in the same scope.
 */

#include "state.h"

#include "filesystem.h"
#include "layout.h"
#include "text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void state_diagnostic(FILE *diagnostics, const char *format, ...) {
    va_list args;

    if (diagnostics == NULL) {
        return;
    }
    va_start(args, format);
    vfprintf(diagnostics, format, args);
    va_end(args);
}

/* Parse scoped state keys without accepting malformed or unknown record families. */
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

/* Compare and copy complete concrete identities; symbolic stable is never valid state. */
static int identity_matches_scope(const PackageIdentity *identity, const PackageScope *scope) {
    return identity != NULL && scope != NULL &&
           strcmp(identity->component, scope->component) == 0 &&
           strcmp(identity->host_platform, scope->host_platform) == 0 &&
           strcmp(identity->target_platform, scope->target_platform) == 0;
}

static int find_installed_index(const CupState *state, const PackageIdentity *identity);
static int find_default_index(const CupState *state, const PackageScope *scope);
static CupError state_set_default_raw(CupState *state, const PackageIdentity *identity);

static int state_counts_within_capacity(const CupState *state) {
    return state != NULL && state->installed_count <= MAX_INSTALLED &&
           state->default_count <= MAX_STATE_DEFAULTS;
}

static CupError identity_scope(const PackageIdentity *identity,
                               PackageScope *scope,
                               FILE *diagnostics) {
    CupError err;

    if (identity == NULL || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_validate(identity, diagnostics);
    if (err != CUP_OK) {
        return err;
    }

    return package_identity_get_scope(identity, scope);
}

/* Validate installed uniqueness and require every default identity to reference an installed
 * package. */
CupError state_validate(const CupState *state, FILE *diagnostics) {
    size_t i;
    size_t j;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!state_counts_within_capacity(state)) {
        state_diagnostic(
            diagnostics, "Error: state record count exceeds its supported capacity.\n");
        return CUP_ERR_STATE_FULL;
    }

    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *installed = &state->installed[i];

        if (package_identity_validate(installed, diagnostics) != CUP_OK) {
            state_diagnostic(
                diagnostics, "Error: installed state identity %zu is invalid.\n", i + 1);
            return CUP_ERR_INCONSISTENT_STATE;
        }

        for (j = 0; j < i; ++j) {
            if (package_identity_equals(&state->installed[j], installed)) {
                state_diagnostic(diagnostics,
                        "Error: duplicate installed state identity '%s:%s@%s'.\n",
                        installed->component,
                        installed->tool,
                        installed->version);
                return CUP_ERR_INCONSISTENT_STATE;
            }
        }
    }

    for (i = 0; i < state->default_count; ++i) {
        const PackageIdentity *default_identity = &state->defaults[i];
        PackageScope scope;

        if (identity_scope(default_identity, &scope, diagnostics) != CUP_OK) {
            state_diagnostic(diagnostics, "Error: default state identity %zu is invalid.\n", i + 1);
            return CUP_ERR_INCONSISTENT_STATE;
        }

        for (j = 0; j < i; ++j) {
            if (identity_matches_scope(&state->defaults[j], &scope)) {
                state_diagnostic(diagnostics,
                        "Error: duplicate default scope for component '%s', "
                        "host '%s', target '%s'.\n",
                        scope.component,
                        scope.host_platform,
                        scope.target_platform);
                return CUP_ERR_INCONSISTENT_STATE;
            }
        }

        if (find_installed_index(state, default_identity) == -1) {
            state_diagnostic(diagnostics,
                    "Error: default state identity '%s@%s' for component '%s', "
                    "host '%s', target '%s' is not installed.\n",
                    default_identity->tool,
                    default_identity->version,
                    default_identity->component,
                    default_identity->host_platform,
                    default_identity->target_platform);
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }

    return CUP_OK;
}

size_t state_count_foreign_hosts(const CupState *state, const char *current_host) {
    size_t count = 0;
    size_t i;

    if (!state_counts_within_capacity(state) || text_is_empty(current_host)) {
        return 0;
    }

    for (i = 0; i < state->installed_count; ++i) {
        if (strcmp(state->installed[i].host_platform, current_host) != 0) {
            count++;
        }
    }
    for (i = 0; i < state->default_count; ++i) {
        if (strcmp(state->defaults[i].host_platform, current_host) != 0) {
            count++;
        }
    }

    return count;
}

CupError state_validate_current_host(const CupState *state,
                                     const char *current_host,
                                     FILE *diagnostics) {
    size_t foreign_count;

    if (state == NULL || text_is_empty(current_host)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        state_diagnostic(
            diagnostics, "Error: state record count exceeds its supported capacity.\n");
        return CUP_ERR_STATE_FULL;
    }

    foreign_count = state_count_foreign_hosts(state, current_host);
    if (foreign_count == 0) {
        return CUP_OK;
    }

    state_diagnostic(diagnostics,
            "Error: state.txt contains %zu record(s) for a foreign host; "
            "run 'cup doctor' before using operational commands.\n",
            foreign_count);
    return CUP_ERR_INCONSISTENT_STATE;
}

/* Parse format=1 atomically into a candidate model; malformed files never leak partial state. */
static CupError parse_state_line(CupState *state, char *line, FILE *diagnostics) {
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

    /* Split the physical record before interpreting its scoped key. */
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
        state_diagnostic(diagnostics, "Error: malformed %s state key '%s'.\n", record_name, key);
        return CUP_ERR_STATE_LOAD;
    }
    if (type == STATE_RECORD_UNKNOWN) {
        state_diagnostic(diagnostics, "Error: unknown state key '%s'.\n", key);
        return CUP_ERR_STATE_LOAD;
    }

    /* Both installed and default records must resolve to one concrete identity. */
    err = package_identity_from_selector(
        &identity, component, host_platform, target_platform, selector, diagnostics);
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    /* Installed records extend the package set; defaults are unique per scope. */
    if (type == STATE_RECORD_INSTALLED) {
        err = state_add_installed(state, &identity);
        if (err == CUP_ERR_STATE_FULL) {
            state_diagnostic(diagnostics,
                             "Error: installed state record capacity was exceeded.\n");
            return err;
        }
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            state_diagnostic(diagnostics,
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
    if (find_default_index(state, &scope) != -1) {
        state_diagnostic(diagnostics,
                "Error: duplicate default for component '%s', host '%s', target '%s'.\n",
                component,
                host_platform,
                target_platform);
        return CUP_ERR_STATE_LOAD;
    }

    err = state_set_default_raw(state, &identity);
    if (err == CUP_ERR_DEFAULT_FULL || err == CUP_ERR_STATE_FULL) {
        state_diagnostic(diagnostics, "Error: default state record capacity was exceeded.\n");
        return err;
    }
    return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

/* Read one bounded state snapshot; parsing failures never expose a partial model. */
static CupError load_state_path(CupState *state,
                                StateFileStatus *status,
                                SystemPathIdentity *source_identity,
                                const char *state_path,
                                FILE *diagnostics) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupError err;
    char line[MAX_STATE_LINE_LEN];
    char expected_header[32];
    int written;
    int has_line;
    int missing;

    if (state == NULL || status == NULL || text_is_empty(state_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(
        state_path, MAX_STATE_FILE_BYTES, &snapshot, &missing);
    if (err != CUP_OK) {
        if (err == CUP_ERR_BUFFER_TOO_SMALL) {
            state_diagnostic(
                diagnostics, "Error: state.txt exceeds its bounded document budget.\n");
            return CUP_ERR_STATE_FULL;
        }
        state_diagnostic(diagnostics, "Error: could not open state file for reading.\n");
        return err;
    }
    if (missing) {
        return CUP_OK;
    }
    if (source_identity != NULL) {
        *source_identity = snapshot.identity;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    if (err != CUP_OK) {
        state_diagnostic(
            diagnostics, "Error: state.txt is not canonical LF-terminated ASCII text.\n");
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_STATE_LOAD;
    }

    written = snprintf(expected_header, sizeof(expected_header), "format=%d", CUP_STATE_FORMAT);
    if (written < 0 || (size_t)written >= sizeof(expected_header)) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_STATE_LOAD;
    }
    err = text_document_read_line(&reader, line, sizeof(line), &has_line);
    if (err != CUP_OK || !has_line || strcmp(line, expected_header) != 0) {
        state_diagnostic(diagnostics,
                         "Error: state.txt must start with the supported 'format=%d' header.\n",
                         CUP_STATE_FORMAT);
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_STATE_LOAD;
    }

    while (1) {
        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK) {
            state_diagnostic(diagnostics, "Error: could not read state file line.\n");
            filesystem_snapshot_release(&snapshot);
            memset(state, 0, sizeof(*state));
            return CUP_ERR_STATE_LOAD;
        }
        if (!has_line) {
            break;
        }

        err = parse_state_line(state, line, diagnostics);
        if (err != CUP_OK) {
            state_diagnostic(
                diagnostics, "Error: invalid state file line %zu.\n", reader.line_number);
            filesystem_snapshot_release(&snapshot);
            memset(state, 0, sizeof(*state));
            if (err == CUP_ERR_STATE_FULL || err == CUP_ERR_DEFAULT_FULL) {
                return err;
            }
            return CUP_ERR_STATE_LOAD;
        }
    }

    filesystem_snapshot_release(&snapshot);
    *status = STATE_FILE_LOADED;
    return CUP_OK;
}

CupError state_load(CupState *state,
                    StateFileStatus *status,
                    SystemPathIdentity *source_identity,
                    FILE *diagnostics) {
    char state_path[MAX_PATH_LEN];
    CupError err;

    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
    if (status != NULL) {
        *status = STATE_FILE_MISSING;
    }
    if (source_identity != NULL) {
        memset(source_identity, 0, sizeof(*source_identity));
    }
    if (state == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_state_path(state_path, sizeof(state_path));
    return err == CUP_OK
               ? load_state_path(state, status, source_identity, state_path, diagnostics)
               : err;
}

static int compare_identity_pointers(const void *left, const void *right) {
    const PackageIdentity *a = *(const PackageIdentity *const *)left;
    const PackageIdentity *b = *(const PackageIdentity *const *)right;
    int result;

    result = strcmp(a->component, b->component);
    if (result == 0) {
        result = strcmp(a->host_platform, b->host_platform);
    }
    if (result == 0) {
        result = strcmp(a->target_platform, b->target_platform);
    }
    if (result == 0) {
        result = strcmp(a->tool, b->tool);
    }
    if (result == 0) {
        result = strcmp(a->version, b->version);
    }
    return result;
}

/* Serialize one canonical installed/default record. */
static CupError write_state_record(FILE *file,
                                   const char *kind,
                                   const PackageIdentity *identity) {
    char selector[MAX_SELECTOR_LEN];

    if (file == NULL || text_is_empty(kind) || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (package_identity_format_selector(identity, selector, sizeof(selector)) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (fprintf(file,
                "%s.%s.%s.%s=%s\n",
                kind,
                identity->component,
                identity->host_platform,
                identity->target_platform,
                selector) < 0) {
        fprintf(stderr, "Error: could not write %s state.\n", kind);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

typedef struct {
    const CupState *state;
    const PackageIdentity *installed[MAX_INSTALLED];
    const PackageIdentity *defaults[MAX_STATE_DEFAULTS];
} StateWriteContext;

static CupError write_state_file(FILE *file, const void *value) {
    const StateWriteContext *context = value;
    CupError err = CUP_OK;
    size_t i;

    if (file == NULL || context == NULL || context->state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (fprintf(file, "format=%d\n", CUP_STATE_FORMAT) < 0) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < context->state->installed_count && err == CUP_OK; ++i) {
        err = write_state_record(file, "installed", context->installed[i]);
    }
    for (i = 0; i < context->state->default_count && err == CUP_OK; ++i) {
        err = write_state_record(file, "default", context->defaults[i]);
    }
    return err;
}

/* Validate, serialize and publish the complete state snapshot. */
CupError state_save(const CupState *state,
                    const SystemPathIdentity *expected_identity,
                    SystemPathIdentity *published_identity) {
    StateWriteContext context;
    SystemPathIdentity expected_copy;
    SystemPathIdentity current_identity;
    const SystemPathIdentity *expected = expected_identity;
    char root[MAX_PATH_LEN];
    char state_path[MAX_PATH_LEN];
    CupError err;
    size_t i;

    if (expected_identity != NULL && published_identity == expected_identity) {
        expected_copy = *expected_identity;
        expected = &expected_copy;
    }
    if (published_identity != NULL) {
        memset(published_identity, 0, sizeof(*published_identity));
    }
    if (state == NULL ||
        (expected != NULL &&
         (!expected->valid || expected->kind != SYSTEM_PATH_REGULAR_FILE))) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(&context, 0, sizeof(context));
    context.state = state;

    /* Validate before touching storage so semantic failures are never reported as I/O errors. */
    err = state_validate(state, stderr);
    if (err == CUP_OK) {
        err = layout_get_root(root, sizeof(root));
    }
    if (err == CUP_OK) {
        err = layout_get_state_path(state_path, sizeof(state_path));
    }
    if (err != CUP_OK) {
        return err;
    }

    /* Sort pointer views rather than mutating the caller's in-memory ordering. */
    for (i = 0; i < state->installed_count; ++i) {
        context.installed[i] = &state->installed[i];
    }
    for (i = 0; i < state->default_count; ++i) {
        context.defaults[i] = &state->defaults[i];
    }
    qsort(context.installed,
          state->installed_count,
          sizeof(context.installed[0]),
          compare_identity_pointers);
    qsort(context.defaults,
          state->default_count,
          sizeof(context.defaults[0]),
          compare_identity_pointers);

    if (expected == NULL) {
        err = filesystem_publish_new_file(
            root, "state", state_path, 0, write_state_file, &context);
    } else {
        err = filesystem_replace_file_if_identity(
            root, "state", state_path, expected, 0, write_state_file, &context);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = system_get_path_identity(state_path, &current_identity);
    if (err != CUP_OK || !current_identity.valid ||
        current_identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_COMMIT;
    }
    if (published_identity != NULL) {
        *published_identity = current_identity;
    }
    return CUP_OK;
}

/* Internal lookup for identities already validated by the caller. */
static int find_installed_index(const CupState *state, const PackageIdentity *identity) {
    size_t i;

    for (i = 0; i < state->installed_count; ++i) {
        if (package_identity_equals(&state->installed[i], identity)) {
            return (int)i;
        }
    }
    return -1;
}

/* Bounded installed-identity lookup and mutation. */
int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    if (!state_counts_within_capacity(state) ||
        package_identity_validate(identity, stderr) != CUP_OK) {
        return -1;
    }
    return find_installed_index(state, identity);
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    CupError err;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }

    err = package_identity_validate(identity, stderr);
    if (err != CUP_OK) {
        return err;
    }
    if (find_installed_index(state, identity) != -1) {
        return CUP_ERR_ALREADY_INSTALLED;
    }
    if (state->installed_count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    state->installed[state->installed_count++] = *identity;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const PackageIdentity *identity) {
    CupError err;
    int index;
    size_t i;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }
    err = package_identity_validate(identity, stderr);
    if (err != CUP_OK) {
        return err;
    }

    index = find_installed_index(state, identity);
    if (index == -1) {
        return CUP_ERR_NOT_INSTALLED;
    }
    for (i = 0; i < state->default_count; ++i) {
        if (package_identity_equals(&state->defaults[i], identity)) {
            fprintf(stderr,
                    "Error: a package selected as default cannot be removed before the "
                    "default is cleared.\n");
            return CUP_ERR_INCONSISTENT_STATE;
        }
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

/* One default identity is allowed for each component, host and target scope. */
static int find_default_index(const CupState *state, const PackageScope *scope) {
    size_t i;

    for (i = 0; i < state->default_count; ++i) {
        if (identity_matches_scope(&state->defaults[i], scope)) {
            return (int)i;
        }
    }
    return -1;
}

const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope) {
    PackageScope validated;
    int index;

    if (!state_counts_within_capacity(state) || scope == NULL ||
        package_scope_init(
            &validated, scope->component, scope->host_platform, scope->target_platform) != CUP_OK) {
        return NULL;
    }
    index = find_default_index(state, &validated);
    return index == -1 ? NULL : &state->defaults[index];
}

static CupError state_set_default_raw(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    CupError err;
    int index;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }

    err = identity_scope(identity, &scope, stderr);
    if (err != CUP_OK) {
        return err;
    }

    index = find_default_index(state, &scope);
    if (index != -1) {
        state->defaults[index] = *identity;
        return CUP_OK;
    }
    if (state->default_count >= MAX_STATE_DEFAULTS) {
        return CUP_ERR_DEFAULT_FULL;
    }

    state->defaults[state->default_count++] = *identity;
    return CUP_OK;
}

CupError state_set_default(CupState *state, const PackageIdentity *identity) {
    CupError err;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }
    err = package_identity_validate(identity, stderr);
    if (err != CUP_OK) {
        return err;
    }
    if (find_installed_index(state, identity) == -1) {
        fprintf(stderr, "Error: a package must be installed before it can become the default.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }
    return state_set_default_raw(state, identity);
}

CupError state_clear_default(CupState *state, const PackageScope *scope) {
    PackageScope validated;
    CupError err;
    int index;
    size_t i;

    if (state == NULL || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }
    err = package_scope_init(
        &validated, scope->component, scope->host_platform, scope->target_platform);
    if (err != CUP_OK) {
        return err;
    }

    index = find_default_index(state, &validated);
    if (index == -1) {
        return CUP_OK;
    }

    for (i = (size_t)index; i + 1 < state->default_count; ++i) {
        state->defaults[i] = state->defaults[i + 1];
    }

    state->default_count--;
    memset(&state->defaults[state->default_count],
           0,
           sizeof(state->defaults[state->default_count]));
    return CUP_OK;
}

CupError state_clear_matching_default(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    const PackageIdentity *current;
    CupError err;

    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!state_counts_within_capacity(state)) {
        return CUP_ERR_STATE_FULL;
    }

    err = identity_scope(identity, &scope, stderr);
    if (err != CUP_OK) {
        return err;
    }

    current = state_get_default(state, &scope);
    if (current == NULL || !package_identity_equals(current, identity)) {
        return CUP_OK;
    }

    return state_clear_default(state, &scope);
}
