/* Dynamic state.txt format-2 persistence. The root owns host identity; persisted records do not. */
#include "state.h"

#include "filesystem.h"
#include "layout.h"
#include "platform.h"
#include "package_selector.h"
#include "text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void state_diagnostic(FILE *diagnostics, const char *format, ...) {
    va_list args;
    if (diagnostics == NULL) return;
    va_start(args, format);
    vfprintf(diagnostics, format, args);
    va_end(args);
}

void state_init(CupState *state) {
    if (state != NULL) memset(state, 0, sizeof(*state));
}

void state_free(CupState *state) {
    if (state == NULL) return;
    free(state->installed);
    memset(state, 0, sizeof(*state));
}

static int identity_matches_scope(const PackageIdentity *identity, const PackageScope *scope) {
    return identity != NULL && scope != NULL &&
           strcmp(identity->component, scope->component) == 0 &&
           strcmp(identity->host_platform, scope->host_platform) == 0 &&
           strcmp(identity->target_platform, scope->target_platform) == 0;
}

static int find_installed_index(const CupState *state, const PackageIdentity *identity) {
    size_t i;
    if (state == NULL || identity == NULL) return -1;
    for (i = 0; i < state->installed_count; ++i) {
        if (package_identity_equals(&state->installed[i], identity)) return (int)i;
    }
    return -1;
}

static int find_default_index(const CupState *state, const PackageScope *scope) {
    size_t i;
    if (state == NULL || scope == NULL) return -1;
    for (i = 0; i < state->default_count; ++i) {
        if (identity_matches_scope(&state->defaults[i], scope)) return (int)i;
    }
    return -1;
}

static CupError reserve_installed(CupState *state, size_t needed) {
    PackageIdentity *items;
    size_t capacity;
    if (state == NULL) return CUP_ERR_INVALID_INPUT;
    if (needed <= state->installed_capacity) return CUP_OK;
    capacity = state->installed_capacity == 0 ? 8u : state->installed_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) return CUP_ERR_STATE_FULL;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*items)) return CUP_ERR_STATE_FULL;
    items = realloc(state->installed, capacity * sizeof(*items));
    if (items == NULL) return CUP_ERR_TEMPORARY;
    state->installed = items;
    state->installed_capacity = capacity;
    return CUP_OK;
}

static CupError identity_scope(const PackageIdentity *identity, PackageScope *scope, FILE *diagnostics) {
    CupError err;
    if (identity == NULL || scope == NULL) return CUP_ERR_INVALID_INPUT;
    err = package_identity_validate(identity, diagnostics);
    if (err != CUP_OK) return err;
    return package_identity_get_scope(identity, scope);
}

CupError state_validate(const CupState *state, FILE *diagnostics) {
    size_t i, j;
    if (state == NULL || state->default_count > MAX_STATE_DEFAULTS ||
        state->installed_count > state->installed_capacity ||
        (state->installed_count > 0 && state->installed == NULL)) {
        return state == NULL ? CUP_ERR_INVALID_INPUT : CUP_ERR_INCONSISTENT_STATE;
    }
    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *installed = &state->installed[i];
        if (package_identity_validate(installed, diagnostics) != CUP_OK) {
            state_diagnostic(diagnostics, "Error: installed state identity %zu is invalid.\n", i + 1);
            return CUP_ERR_INCONSISTENT_STATE;
        }
        for (j = 0; j < i; ++j) {
            if (package_identity_equals(&state->installed[j], installed)) {
                state_diagnostic(diagnostics, "Error: duplicate installed state identity '%s:%s@%s'.\n",
                                 installed->component, installed->tool, installed->version);
                return CUP_ERR_INCONSISTENT_STATE;
            }
        }
    }
    for (i = 0; i < state->default_count; ++i) {
        const PackageIdentity *def = &state->defaults[i];
        PackageScope scope;
        if (identity_scope(def, &scope, diagnostics) != CUP_OK) return CUP_ERR_INCONSISTENT_STATE;
        for (j = 0; j < i; ++j) {
            if (identity_matches_scope(&state->defaults[j], &scope)) {
                state_diagnostic(diagnostics, "Error: duplicate default scope for component '%s', target '%s'.\n",
                                 scope.component, scope.target_platform);
                return CUP_ERR_INCONSISTENT_STATE;
            }
        }
        if (find_installed_index(state, def) < 0) {
            state_diagnostic(diagnostics, "Error: default state identity '%s@%s' for component '%s', target '%s' is not installed.\n",
                             def->tool, def->version, def->component, def->target_platform);
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }
    return CUP_OK;
}



CupError state_measure_persistent(const CupState *state, size_t *size) {
    size_t total = sizeof("format=2\n") - 1u;
    size_t i;
    CupError err;

    if (state == NULL || size == NULL) return CUP_ERR_INVALID_INPUT;
    *size = 0;
    err = state_validate(state, NULL);
    if (err != CUP_OK) return err;
    for (i = 0; i < state->installed_count + state->default_count; ++i) {
        const PackageIdentity *identity =
            i < state->installed_count ? &state->installed[i]
                                       : &state->defaults[i - state->installed_count];
        const char *kind = i < state->installed_count ? "installed" : "default";
        char selector[MAX_SELECTOR_LEN];
        char line[MAX_STATE_LINE_LEN];
        int written;

        err = package_identity_format_selector(identity, selector, sizeof(selector));
        if (err != CUP_OK) return err;
        written = snprintf(line,
                           sizeof(line),
                           "%s.%s.%s=%s\n",
                           kind,
                           identity->component,
                           identity->target_platform,
                           selector);
        if (written < 0 || (size_t)written >= sizeof(line)) return CUP_ERR_STATE_FULL;
        if (total > MAX_STATE_FILE_BYTES - (size_t)written) return CUP_ERR_STATE_FULL;
        total += (size_t)written;
    }
    *size = total;
    return CUP_OK;
}

size_t state_count_foreign_hosts(const CupState *state, const char *current_host) {
    size_t count = 0, i;
    if (state == NULL || text_is_empty(current_host)) return 0;
    for (i = 0; i < state->installed_count; ++i)
        if (strcmp(state->installed[i].host_platform, current_host) != 0) ++count;
    for (i = 0; i < state->default_count; ++i)
        if (strcmp(state->defaults[i].host_platform, current_host) != 0) ++count;
    return count;
}

CupError state_validate_current_host(const CupState *state, const char *current_host, FILE *diagnostics) {
    size_t foreign_count;
    if (state == NULL || text_is_empty(current_host)) return CUP_ERR_INVALID_INPUT;
    foreign_count = state_count_foreign_hosts(state, current_host);
    if (foreign_count == 0) return CUP_OK;
    state_diagnostic(diagnostics, "Error: in-memory state contains %zu record(s) for a foreign host.\n", foreign_count);
    return CUP_ERR_INCONSISTENT_STATE;
}

typedef enum { STATE_RECORD_UNKNOWN, STATE_RECORD_INSTALLED, STATE_RECORD_DEFAULT } StateRecordType;

static CupError parse_state_key(const char *key, StateRecordType *type,
                                char *component, size_t component_size,
                                char *target, size_t target_size) {
    static const char installed_prefix[] = "installed.";
    static const char default_prefix[] = "default.";
    TextBuffer parts[2];
    char body[MAX_STATE_LINE_LEN];
    const char *scope;
    if (text_is_empty(key) || type == NULL || component == NULL || target == NULL) return CUP_ERR_INVALID_INPUT;
    if (strncmp(key, installed_prefix, sizeof(installed_prefix)-1) == 0) {
        *type = STATE_RECORD_INSTALLED; scope = key + sizeof(installed_prefix)-1;
    } else if (strncmp(key, default_prefix, sizeof(default_prefix)-1) == 0) {
        *type = STATE_RECORD_DEFAULT; scope = key + sizeof(default_prefix)-1;
    } else { *type = STATE_RECORD_UNKNOWN; return CUP_OK; }
    if (text_copy(body, sizeof(body), scope) != CUP_OK) return CUP_ERR_STATE_LOAD;
    parts[0] = (TextBuffer){component, component_size};
    parts[1] = (TextBuffer){target, target_size};
    return text_split_exact(body, '.', parts, 2) == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

static CupError state_set_default_raw(CupState *state, const PackageIdentity *identity);

static CupError parse_state_line(CupState *state, char *line, const char *host, FILE *diagnostics) {
    PackageIdentity identity;
    PackageScope scope;
    StateRecordType type = STATE_RECORD_UNKNOWN;
    CupError err;
    char key[MAX_STATE_LINE_LEN], selector[MAX_SELECTOR_LEN];
    char component[MAX_IDENTIFIER_LEN], target[MAX_PLATFORM_LEN];
    if (state == NULL || line == NULL || text_is_empty(host)) return CUP_ERR_INVALID_INPUT;
    if (text_parse_key_value(line, key, sizeof(key), selector, sizeof(selector)) != CUP_OK) return CUP_ERR_STATE_LOAD;
    err = parse_state_key(key, &type, component, sizeof(component), target, sizeof(target));
    if (err != CUP_OK || type == STATE_RECORD_UNKNOWN) return CUP_ERR_STATE_LOAD;
    err = package_identity_from_selector(&identity, component, host, target, selector, diagnostics);
    if (err != CUP_OK) return CUP_ERR_STATE_LOAD;
    if (type == STATE_RECORD_INSTALLED) {
        err = state_add_installed(state, &identity);
        return err == CUP_OK ? CUP_OK : (err == CUP_ERR_TEMPORARY ? err : CUP_ERR_STATE_LOAD);
    }
    if (package_identity_get_scope(&identity, &scope) != CUP_OK || find_default_index(state, &scope) >= 0)
        return CUP_ERR_STATE_LOAD;
    err = state_set_default_raw(state, &identity);
    return err == CUP_OK ? CUP_OK : CUP_ERR_STATE_LOAD;
}

static CupError load_state_path(CupState *state, StateFileStatus *status,
                                SystemPathIdentity *source_identity, const char *path,
                                FILE *diagnostics) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupState candidate;
    CupError err;
    char line[MAX_STATE_LINE_LEN], header[32], host[MAX_PLATFORM_LEN];
    int has_line, missing, n;
    state_init(&candidate);
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, MAX_STATE_FILE_BYTES, &snapshot, &missing);
    if (err != CUP_OK) {
        state_diagnostic(diagnostics, err == CUP_ERR_BUFFER_TOO_SMALL ?
            "Error: state.txt exceeds its 4 MiB document budget.\n" :
            "Error: could not open state file for reading.\n");
        return err == CUP_ERR_BUFFER_TOO_SMALL ? CUP_ERR_STATE_FULL : err;
    }
    if (missing) return CUP_OK;
    if (source_identity != NULL) *source_identity = snapshot.identity;
    if (platform_get_host(host, sizeof(host)) != CUP_OK) { filesystem_snapshot_release(&snapshot); return CUP_ERR_STATE_LOAD; }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    n = snprintf(header, sizeof(header), "format=%d", CUP_STATE_FORMAT);
    if (err != CUP_OK || n < 0 || (size_t)n >= sizeof(header) ||
        text_document_read_line(&reader, line, sizeof(line), &has_line) != CUP_OK ||
        !has_line || strcmp(line, header) != 0) {
        filesystem_snapshot_release(&snapshot); return CUP_ERR_STATE_LOAD;
    }
    while (1) {
        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK) { err = CUP_ERR_STATE_LOAD; goto fail; }
        if (!has_line) break;
        err = parse_state_line(&candidate, line, host, diagnostics);
        if (err != CUP_OK) goto fail;
    }
    err = state_validate(&candidate, diagnostics);
    if (err != CUP_OK) { err = CUP_ERR_STATE_LOAD; goto fail; }
    filesystem_snapshot_release(&snapshot);
    state_free(state);
    *state = candidate;
    *status = STATE_FILE_LOADED;
    return CUP_OK;
fail:
    filesystem_snapshot_release(&snapshot);
    state_free(&candidate);
    return err;
}

CupError state_load(CupState *state, StateFileStatus *status,
                    SystemPathIdentity *source_identity, FILE *diagnostics) {
    char path[MAX_PATH_LEN];
    CupError err;
    if (state == NULL || status == NULL) return CUP_ERR_INVALID_INPUT;
    *status = STATE_FILE_MISSING;
    if (source_identity != NULL) memset(source_identity, 0, sizeof(*source_identity));
    err = layout_get_state_path(path, sizeof(path));
    return err == CUP_OK ? load_state_path(state, status, source_identity, path, diagnostics) : err;
}

static int compare_identity_pointers(const void *lv, const void *rv) {
    const PackageIdentity *a = *(const PackageIdentity *const *)lv;
    const PackageIdentity *b = *(const PackageIdentity *const *)rv;
    int r = strcmp(a->component, b->component);
    if (r == 0) r = strcmp(a->target_platform, b->target_platform);
    if (r == 0) r = strcmp(a->tool, b->tool);
    if (r == 0) {
        int version_result = 0;
        r = package_release_compare(a->version, b->version, &version_result) == CUP_OK
                ? version_result
                : strcmp(a->version, b->version);
    }
    return r;
}

static CupError write_state_record(FILE *file, const char *kind, const PackageIdentity *identity) {
    char selector[MAX_SELECTOR_LEN];
    if (package_identity_format_selector(identity, selector, sizeof(selector)) != CUP_OK) return CUP_ERR_INCONSISTENT_STATE;
    return fprintf(file, "%s.%s.%s=%s\n", kind, identity->component,
                   identity->target_platform, selector) < 0 ? CUP_ERR_FILESYSTEM : CUP_OK;
}

typedef struct {
    const CupState *state;
    const PackageIdentity **installed;
    const PackageIdentity *defaults[MAX_STATE_DEFAULTS];
} StateWriteContext;

static CupError write_state_file(FILE *file, const void *value) {
    const StateWriteContext *c = value;
    CupError err = CUP_OK;
    size_t i;
    if (fprintf(file, "format=%d\n", CUP_STATE_FORMAT) < 0) return CUP_ERR_FILESYSTEM;
    for (i=0; i<c->state->installed_count && err==CUP_OK; ++i) err = write_state_record(file,"installed",c->installed[i]);
    for (i=0; i<c->state->default_count && err==CUP_OK; ++i) err = write_state_record(file,"default",c->defaults[i]);
    return err;
}

CupError state_save(const CupState *state, const SystemPathIdentity *expected_identity,
                    SystemPathIdentity *published_identity) {
    StateWriteContext c = {0};
    SystemPathIdentity expected_copy, current_identity;
    const SystemPathIdentity *expected = expected_identity;
    char root[MAX_PATH_LEN], path[MAX_PATH_LEN], host[MAX_PLATFORM_LEN];
    CupError err;
    size_t i;
    if (state == NULL) return CUP_ERR_INVALID_INPUT;
    if (expected_identity != NULL && published_identity == expected_identity) { expected_copy=*expected_identity; expected=&expected_copy; }
    if (published_identity != NULL) memset(published_identity,0,sizeof(*published_identity));
    err = state_validate(state, stderr);
    if (err != CUP_OK) return err;
    {
        size_t persistent_size;
        err = state_measure_persistent(state, &persistent_size);
        if (err != CUP_OK) return err;
    }
    if (platform_get_host(host,sizeof(host)) != CUP_OK || state_validate_current_host(state,host,stderr) != CUP_OK)
        return CUP_ERR_INCONSISTENT_STATE;
    c.state = state;
    if (state->installed_count > 0) {
        c.installed = malloc(state->installed_count * sizeof(*c.installed));
        if (c.installed == NULL) return CUP_ERR_TEMPORARY;
    }
    for (i=0;i<state->installed_count;++i) c.installed[i]=&state->installed[i];
    for (i=0;i<state->default_count;++i) c.defaults[i]=&state->defaults[i];
    qsort(c.installed,state->installed_count,sizeof(c.installed[0]),compare_identity_pointers);
    qsort(c.defaults,state->default_count,sizeof(c.defaults[0]),compare_identity_pointers);
    if (layout_get_root(root,sizeof(root)) != CUP_OK || layout_get_state_path(path,sizeof(path)) != CUP_OK) { free(c.installed); return CUP_ERR_FILESYSTEM; }
    if (expected == NULL) err = filesystem_publish_new_file(root,"state",path,0,write_state_file,&c);
    else err = filesystem_replace_file_if_identity(root,"state",path,expected,0,write_state_file,&c);
    free(c.installed);
    if (err != CUP_OK || published_identity == NULL) return err;
    if (system_get_path_identity(path,&current_identity) != CUP_OK || !current_identity.valid || current_identity.kind != SYSTEM_PATH_REGULAR_FILE)
        return CUP_ERR_COMMIT;
    *published_identity=current_identity;
    return CUP_OK;
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    if (state == NULL || package_identity_validate(identity,NULL) != CUP_OK) return -1;
    return find_installed_index(state,identity);
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    CupError err;
    if (state == NULL) return CUP_ERR_INVALID_INPUT;
    err = package_identity_validate(identity,stderr);
    if (err != CUP_OK) return err;
    if (find_installed_index(state,identity)>=0) return CUP_ERR_ALREADY_INSTALLED;
    err = reserve_installed(state,state->installed_count+1);
    if (err != CUP_OK) return err;
    state->installed[state->installed_count++] = *identity;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const PackageIdentity *identity) {
    int index;
    size_t i;
    if (state == NULL || package_identity_validate(identity,stderr) != CUP_OK) return CUP_ERR_INVALID_INPUT;
    index=find_installed_index(state,identity);
    if (index<0) return CUP_ERR_NOT_INSTALLED;
    for (i=0;i<state->default_count;++i) if (package_identity_equals(&state->defaults[i],identity)) return CUP_ERR_INCONSISTENT_STATE;
    if ((size_t)index + 1 < state->installed_count)
        memmove(&state->installed[index],&state->installed[index+1],(state->installed_count-(size_t)index-1)*sizeof(state->installed[0]));
    --state->installed_count;
    if (state->installed != NULL) memset(&state->installed[state->installed_count],0,sizeof(state->installed[0]));
    return CUP_OK;
}

const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope) {
    int index;
    if (state == NULL || scope == NULL) return NULL;
    index=find_default_index(state,scope);
    return index<0 ? NULL : &state->defaults[index];
}


CupError state_get_tool_reference(const CupState *state,
                                  const PackageScope *scope,
                                  const char *tool,
                                  PackageIdentity *reference,
                                  int *reference_is_default) {
    const PackageIdentity *current_default;
    const PackageIdentity *best = NULL;
    size_t i;

    if (state == NULL || scope == NULL || text_is_empty(tool) || reference == NULL ||
        reference_is_default == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(reference, 0, sizeof(*reference));
    *reference_is_default = 0;

    current_default = state_get_default(state, scope);
    if (current_default != NULL && strcmp(current_default->tool, tool) == 0) {
        *reference = *current_default;
        *reference_is_default = 1;
        return CUP_OK;
    }

    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *candidate = &state->installed[i];
        int comparison;
        CupError err;

        if (!identity_matches_scope(candidate, scope) || strcmp(candidate->tool, tool) != 0) {
            continue;
        }
        if (best == NULL) {
            best = candidate;
            continue;
        }
        err = package_release_compare(candidate->version, best->version, &comparison);
        if (err != CUP_OK) {
            return err;
        }
        if (comparison > 0) {
            best = candidate;
        }
    }

    if (best == NULL) {
        return CUP_ERR_NOT_INSTALLED;
    }
    *reference = *best;
    return CUP_OK;
}

static CupError state_set_default_raw(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    int index;
    if (state == NULL || identity_scope(identity,&scope,stderr) != CUP_OK) return CUP_ERR_INVALID_INPUT;
    index=find_default_index(state,&scope);
    if (index>=0) { state->defaults[index]=*identity; return CUP_OK; }
    if (state->default_count>=MAX_STATE_DEFAULTS) return CUP_ERR_DEFAULT_FULL;
    state->defaults[state->default_count++]=*identity;
    return CUP_OK;
}

CupError state_set_default(CupState *state, const PackageIdentity *identity) {
    if (state == NULL || package_identity_validate(identity,stderr) != CUP_OK) return CUP_ERR_INVALID_INPUT;
    if (find_installed_index(state,identity)<0) return CUP_ERR_INCONSISTENT_STATE;
    return state_set_default_raw(state,identity);
}

CupError state_clear_default(CupState *state, const PackageScope *scope) {
    int index;
    size_t i;
    if (state == NULL || scope == NULL) return CUP_ERR_INVALID_INPUT;
    index=find_default_index(state,scope);
    if (index<0) return CUP_OK;
    for (i=(size_t)index;i+1<state->default_count;++i) state->defaults[i]=state->defaults[i+1];
    --state->default_count;
    memset(&state->defaults[state->default_count],0,sizeof(state->defaults[0]));
    return CUP_OK;
}

CupError state_clear_matching_default(CupState *state, const PackageIdentity *identity) {
    PackageScope scope;
    const PackageIdentity *current;
    if (state == NULL || identity_scope(identity,&scope,stderr) != CUP_OK) return CUP_ERR_INVALID_INPUT;
    current=state_get_default(state,&scope);
    if (current==NULL || !package_identity_equals(current,identity)) return CUP_OK;
    return state_clear_default(state,&scope);
}
