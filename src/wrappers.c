/*
 * Builds immutable managed-wrapper plans from validated default packages and applies or diagnoses
 * the exact wrapper set under the canonical bin directory.
 */

#include "wrappers.h"

#include "filesystem.h"
#include "package_metadata.h"
#include "layout.h"
#include "package.h"
#include "path.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const WrapperPlan *wrappers;
    const char *binary_name;
    int names_case_sensitive;
    int remove_stale;
    size_t *issue_count;
    int *unexpected_found;
} ScanContext;

typedef enum {
    WRAPPER_DESTINATION_MISSING,
    WRAPPER_DESTINATION_VALID,
    WRAPPER_DESTINATION_STALE_CONTENT,
    WRAPPER_DESTINATION_WRONG_KIND,
    WRAPPER_DESTINATION_WRONG_MODE
} WrapperDestinationState;

/* Wrapper-plan ownership. Plans are derived data and can be discarded or rebuilt without changing
 * authoritative state. */
void wrapper_plan_init(WrapperPlan *plan) {
    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
}

void wrapper_plan_free(WrapperPlan *wrappers) {
    if (wrappers == NULL) {
        return;
    }

    free(wrappers->items);
    memset(wrappers, 0, sizeof(*wrappers));
}

static int names_equal_case_insensitive(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int names_equal(const char *left, const char *right, int case_sensitive) {
    return case_sensitive ? strcmp(left, right) == 0
                          : names_equal_case_insensitive(left, right);
}

static CupError wrapper_names_case_sensitive(int *case_sensitive) {
    char root[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char alternate[MAX_PATH_LEN];
    char alternate_name[sizeof(CUP_ROOT_MARKER_FILENAME)];
    SystemPathIdentity marker_identity;
    SystemPathIdentity alternate_identity;
    size_t i;
    CupError err;

    if (case_sensitive == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_root(root, sizeof(root));
    if (err == CUP_OK) {
        err = path_join(marker, sizeof(marker), root, CUP_ROOT_MARKER_FILENAME);
    }
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < sizeof(CUP_ROOT_MARKER_FILENAME); ++i) {
        unsigned char value = (unsigned char)CUP_ROOT_MARKER_FILENAME[i];

        if (value >= 'a' && value <= 'z') {
            alternate_name[i] = (char)(value - ('a' - 'A'));
        } else if (value >= 'A' && value <= 'Z') {
            alternate_name[i] = (char)(value + ('a' - 'A'));
        } else {
            alternate_name[i] = (char)value;
        }
    }
    err = path_join(alternate, sizeof(alternate), root, alternate_name);
    if (err == CUP_OK) {
        err = system_get_path_identity(marker, &marker_identity);
    }
    if (err == CUP_OK) {
        err = system_get_path_identity(alternate, &alternate_identity);
    }
    if (err != CUP_OK || !marker_identity.valid ||
        marker_identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }

    *case_sensitive = !alternate_identity.valid ||
                      !system_path_identity_equal(&marker_identity, &alternate_identity);
    return CUP_OK;
}

static int find_wrapper(const WrapperPlan *wrappers, const char *name, int case_sensitive) {
    size_t i;

    for (i = 0; i < wrappers->count; ++i) {
        if (names_equal(wrappers->items[i].name, name, case_sensitive)) {
            return (int)i;
        }
    }

    return -1;
}

static CupError add_wrapper(WrapperPlan *wrappers,
                            const char *name,
                            const char *target,
                            int case_sensitive) {
    WrapperSpec *items;
    size_t capacity;
    int existing;

    existing = find_wrapper(wrappers, name, case_sensitive);
    if (existing >= 0) {
        if (strcmp(wrappers->items[existing].target, target) == 0) {
            return CUP_OK;
        }

        fprintf(
            stderr, "Error: wrapper '%s' is declared by more than one default package.\n", name);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (wrappers->count == wrappers->capacity) {
        if (wrappers->capacity > SIZE_MAX / 2) {
            return CUP_ERR_TEMPORARY;
        }
        capacity = wrappers->capacity == 0 ? 16 : wrappers->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*items)) {
            return CUP_ERR_TEMPORARY;
        }
        items = realloc(wrappers->items, capacity * sizeof(*items));
        if (items == NULL) {
            return CUP_ERR_TEMPORARY;
        }
        wrappers->items = items;
        wrappers->capacity = capacity;
    }

    if (text_copy(wrappers->items[wrappers->count].name,
                  sizeof(wrappers->items[wrappers->count].name),
                  name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (text_copy(wrappers->items[wrappers->count].target,
                  sizeof(wrappers->items[wrappers->count].target),
                  target) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    wrappers->count++;
    return CUP_OK;
}

/* Public-name selection uses the case semantics observed in the selected root so wrapper names
 * cannot alias on that filesystem. */
static CupError build_wrapper_name(char *buffer,
                                   size_t size,
                                   const PackageIdentity *default_identity,
                                   const char *entry_name) {
    CupError err;

    if (strcmp(default_identity->host_platform, default_identity->target_platform) == 0) {
        err = text_copy(buffer, size, entry_name);
    } else {
        err = text_format(buffer, size, "%s-%s", default_identity->target_platform, entry_name);
    }
    if (err != CUP_OK) {
        return err;
    }

#if defined(_WIN32)
    {
        PublicCommandName command_name;

        err = text_format(command_name, sizeof(command_name), "%s.cmd", buffer);
        if (err != CUP_OK) {
            return err;
        }
        return text_copy(buffer, size, command_name);
    }
#else
    return CUP_OK;
#endif
}

static CupError collect_package_commands(WrapperPlan *wrappers,
                                         const PackageIdentity *default_identity,
                                         int names_case_sensitive) {
    ValidatedPackage package;
    PackageCommand command;
    CupError err;
    char install_path[MAX_PATH_LEN];
    size_t cursor = 0;

    validated_package_init(&package);

    err = package_identity_validate(default_identity, stderr);
    if (err != CUP_OK) {
        goto done;
    }
    err = layout_build_install_path(install_path, sizeof(install_path), default_identity);
    if (err != CUP_OK) {
        goto done;
    }
    err = validated_package_load(&package, install_path, default_identity, stderr);
    if (err != CUP_OK) {
        goto done;
    }

    while (package_metadata_next_command(&package.metadata, &command, &cursor)) {
        PublicCommandName name;
        char target[MAX_PATH_LEN];

        err = build_wrapper_name(name, sizeof(name), default_identity, command.name);
        if (err != CUP_OK) {
            goto done;
        }
        err = text_format(target,
                          sizeof(target),
                          "../components/%s/%s/%s/%s/%s/%s",
                          default_identity->component,
                          default_identity->tool,
                          default_identity->host_platform,
                          default_identity->target_platform,
                          default_identity->version,
                          command.path);
        if (err != CUP_OK) {
            goto done;
        }

        if (strcmp(default_identity->host_platform, default_identity->target_platform) == 0 &&
            names_equal(command.name, "cup", names_case_sensitive)) {
            fprintf(
                stderr, "Error: package command '%s' conflicts with cup itself.\n", command.name);
            err = CUP_ERR_INCONSISTENT_STATE;
            goto done;
        }

        err = add_wrapper(wrappers, name, target, names_case_sensitive);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = CUP_OK;

done:
    validated_package_free(&package);
    return err;
}

static CupError collect_wrappers(const CupState *state,
                                 WrapperPlan *wrappers,
                                 int names_case_sensitive) {
    CupError err;
    char host[MAX_PLATFORM_LEN];
    size_t i;

    if (state == NULL || wrappers == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = platform_get_host(host, sizeof(host));
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < state->default_count; ++i) {
        if (strcmp(state->defaults[i].host_platform, host) != 0) {
            continue;
        }

        err = collect_package_commands(wrappers,
                                       &state->defaults[i],
                                       names_case_sensitive);
        if (err != CUP_OK) {
            wrapper_plan_free(wrappers);
            return err;
        }
    }

    return CUP_OK;
}

static CupError append_text(char *buffer, size_t size, size_t *length, const char *text) {
    size_t text_length;

    if (buffer == NULL || length == NULL || text == NULL || *length >= size) {
        return CUP_ERR_INVALID_INPUT;
    }

    text_length = strlen(text);
    if (text_length >= size - *length) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    buffer[*length] = '\0';
    return CUP_OK;
}

static CupError append_character(char *buffer, size_t size, size_t *length, char character) {
    if (buffer == NULL || length == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (*length >= size - 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    buffer[*length] = character;
    (*length)++;
    buffer[*length] = '\0';
    return CUP_OK;
}

#if !defined(_WIN32)
static CupError append_shell_quoted(char *buffer, size_t size, size_t *length, const char *text) {
    const char *cursor;
    CupError err;

    err = append_text(buffer, size, length, "'");
    if (err != CUP_OK) {
        return err;
    }

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\'') {
            err = append_text(buffer, size, length, "'\"'\"'");
        } else {
            err = append_character(buffer, size, length, *cursor);
        }
        if (err != CUP_OK) {
            return err;
        }
    }

    return append_text(buffer, size, length, "'");
}
#endif

/* Script generation. Arguments and paths are quoted for the destination shell rather than
 * concatenated as raw text. */
static CupError build_wrapper_content(const WrapperSpec *wrapper,
                                      char **content,
                                      size_t *content_size) {
    size_t capacity;
    size_t length = 0;
    size_t target_length;
    char *buffer;
    CupError err;

    if (wrapper == NULL || content == NULL || content_size == NULL ||
        text_is_empty(wrapper->target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *content = NULL;
    *content_size = 0;
    target_length = strlen(wrapper->target);
    if (target_length > (SIZE_MAX - 512) / 5) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    capacity = target_length * 5 + 512;
    buffer = calloc(capacity, 1);
    if (buffer == NULL) {
        return CUP_ERR_TEMPORARY;
    }

#if defined(_WIN32)
    {
        const char *cursor;

        err = append_text(buffer,
                          capacity,
                          &length,
                          "@echo off\r\n"
                          "setlocal DisableDelayedExpansion\r\n"
                          "\"%~dp0");
        if (err == CUP_OK) {
            for (cursor = wrapper->target; *cursor != '\0'; ++cursor) {
                if (*cursor == '%') {
                    err = append_text(buffer, capacity, &length, "%%");
                } else {
                    err = append_character(
                        buffer, capacity, &length, *cursor == '/' ? '\\' : *cursor);
                }
                if (err != CUP_OK) {
                    break;
                }
            }
        }
        if (err == CUP_OK) {
            err = append_text(buffer, capacity, &length, "\" %*\r\n");
        }
    }
#else
    err = append_text(buffer,
                      capacity,
                      &length,
                      "#!/bin/sh\n"
                      "wrapper_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd) || exit 1\n"
                      "exec \"$wrapper_dir\"/");
    if (err == CUP_OK) {
        err = append_shell_quoted(buffer, capacity, &length, wrapper->target);
    }
    if (err == CUP_OK) {
        err = append_text(buffer, capacity, &length, " \"$@\"\n");
    }
#endif

    if (err != CUP_OK) {
        free(buffer);
        return err;
    }

    *content = buffer;
    *content_size = length;
    return CUP_OK;
}

typedef struct {
    const char *content;
    size_t size;
} WrapperWriteContext;

static CupError write_wrapper_file(FILE *file, const void *value) {
    const WrapperWriteContext *context = value;

    if (file == NULL || context == NULL || context->content == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return fwrite(context->content, 1, context->size, file) == context->size
               ? CUP_OK
               : CUP_ERR_FILESYSTEM;
}

static CupError write_wrapper(const char *bin_dir, const WrapperSpec *wrapper) {
    WrapperWriteContext context;
    CupError err;
    char destination[MAX_PATH_LEN];
    char *content = NULL;
    size_t content_size = 0;

    if (text_is_empty(bin_dir) || wrapper == NULL || text_is_empty(wrapper->name) ||
        text_is_empty(wrapper->target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(destination, sizeof(destination), bin_dir, wrapper->name);
    if (err != CUP_OK) {
        return err;
    }

    err = build_wrapper_content(wrapper, &content, &content_size);
    if (err != CUP_OK) {
        return err;
    }

    context.content = content;
    context.size = content_size;
    err = filesystem_replace_file_atomically(
        bin_dir, "wrapper", destination, 1, write_wrapper_file, &context);
    free(content);
    return err;
}

static int wrapper_is_expected(const WrapperPlan *wrappers,
                               const char *name,
                               int names_case_sensitive) {
    return find_wrapper(wrappers, name, names_case_sensitive) >= 0;
}

/* Installed-wrapper inspection. Unexpected, stale or modified files are reported without becoming a
 * new source of truth. */
static CupError scan_bin_entry(const char *path,
                               SystemPathKind kind,
                               const SystemPathIdentity *identity,
                               void *userdata) {
    ScanContext *context = userdata;
    const char *name;

    if (identity == NULL || !identity->valid || identity->kind != kind) {
        return CUP_ERR_FILESYSTEM;
    }
    if (text_is_empty(path) || context == NULL || context->wrappers == NULL ||
        text_is_empty(context->binary_name) ||
        (!context->remove_stale && context->issue_count == NULL &&
         context->unexpected_found == NULL)) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL ||
        names_equal(name, context->binary_name, context->names_case_sensitive) ||
        wrapper_is_expected(context->wrappers, name, context->names_case_sensitive)) {
        return CUP_OK;
    }

    if (context->remove_stale) {
        return system_remove_path_if_identity(path, identity, NULL);
    }

    if (context->unexpected_found != NULL) {
        *context->unexpected_found = 1;
    }
    if (context->issue_count != NULL) {
        printf("Issue: stale or unmanaged wrapper '%s' exists.\n", path);
        (*context->issue_count)++;
    }
    return CUP_OK;
}

static CupError classify_wrapper(const char *bin_dir,
                                 const WrapperSpec *wrapper,
                                 WrapperDestinationState *state) {
    CupError err;
    SystemPathKind kind;
    PersistentFileSnapshot snapshot;
    char path[MAX_PATH_LEN];
    char *expected = NULL;
    size_t expected_size = 0;
    int missing;

    if (text_is_empty(bin_dir) || wrapper == NULL || state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *state = WRAPPER_DESTINATION_MISSING;

    if (path_join(path, sizeof(path), bin_dir, wrapper->name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        *state = WRAPPER_DESTINATION_WRONG_KIND;
        return CUP_OK;
    }

#if !defined(_WIN32)
    {
        int executable;

        err = system_is_executable(path, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable) {
            *state = WRAPPER_DESTINATION_WRONG_MODE;
            return CUP_OK;
        }
    }
#endif

    err = build_wrapper_content(wrapper, &expected, &expected_size);
    if (err != CUP_OK || expected == NULL || expected_size == 0) {
        free(expected);
        return err != CUP_OK ? err : CUP_ERR_INCONSISTENT_STATE;
    }

    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, expected_size + 1u, &snapshot, &missing);
    if (err == CUP_ERR_BUFFER_TOO_SMALL) {
        free(expected);
        *state = WRAPPER_DESTINATION_STALE_CONTENT;
        return CUP_OK;
    }
    if (err != CUP_OK || missing) {
        free(expected);
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }

    *state = snapshot.size == expected_size &&
                     memcmp(snapshot.data, expected, expected_size) == 0
                 ? WRAPPER_DESTINATION_VALID
                 : WRAPPER_DESTINATION_STALE_CONTENT;
    filesystem_snapshot_release(&snapshot);
    free(expected);
    return CUP_OK;
}

static CupError compare_wrapper(const char *bin_dir, const WrapperSpec *wrapper, int *matches) {
    WrapperDestinationState state;
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;
    err = classify_wrapper(bin_dir, wrapper, &state);
    if (err == CUP_OK) {
        *matches = state == WRAPPER_DESTINATION_VALID;
    }
    return err;
}

static CupError remove_wrong_kind_destination(const char *bin_dir, const WrapperSpec *wrapper) {
    SystemPathIdentity identity;
    CupError err;
    char path[MAX_PATH_LEN];

    if (path_join(path, sizeof(path), bin_dir, wrapper->name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    err = system_get_path_identity(path, &identity);
    if (err != CUP_OK || !identity.valid || identity.kind == SYSTEM_PATH_REGULAR_FILE) {
        return err;
    }
    return system_remove_path_if_identity(path, &identity, NULL);
}

/* Public plan construction, application and diagnosis. */
CupError wrapper_plan_build_default(WrapperPlan *plan, const PackageIdentity *default_identity) {
    int names_case_sensitive;
    CupError err;

    if (plan == NULL || default_identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    wrapper_plan_free(plan);
    err = wrapper_names_case_sensitive(&names_case_sensitive);
    return err == CUP_OK
               ? collect_package_commands(plan, default_identity, names_case_sensitive)
               : err;
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    int names_case_sensitive;
    CupError err;

    if (plan == NULL || state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    wrapper_plan_free(plan);
    err = wrapper_names_case_sensitive(&names_case_sensitive);
    return err == CUP_OK ? collect_wrappers(state, plan, names_case_sensitive) : err;
}

/* Application and diagnostics. The complete plan is written deterministically, then compared by
 * doctor when requested. */
CupError wrapper_plan_apply(const WrapperPlan *wrappers) {
    ScanContext scan;
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    char binary_path[MAX_PATH_LEN];
    const char *binary_name;
    int names_case_sensitive;
    size_t i;

    if (wrappers == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        filesystem_ensure_directory(bin_dir) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    err = wrapper_names_case_sensitive(&names_case_sensitive);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < wrappers->count; ++i) {
        err = remove_wrong_kind_destination(bin_dir, &wrappers->items[i]);
        if (err != CUP_OK) {
            return err;
        }
        err = write_wrapper(bin_dir, &wrappers->items[i]);
        if (err != CUP_OK) {
            return err;
        }
    }

    binary_name = path_last_segment(binary_path);
    if (binary_name == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    scan.wrappers = wrappers;
    scan.binary_name = binary_name;
    scan.names_case_sensitive = names_case_sensitive;
    scan.remove_stale = 1;
    scan.issue_count = NULL;
    scan.unexpected_found = NULL;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}

CupError wrapper_plan_entries_match(const WrapperPlan *wrappers, int *matches) {
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    size_t i;

    if (wrappers == NULL || matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < wrappers->count; ++i) {
        int wrapper_matches;

        err = compare_wrapper(bin_dir, &wrappers->items[i], &wrapper_matches);
        if (err != CUP_OK) {
            return err;
        }
        if (!wrapper_matches) {
            return CUP_OK;
        }
    }

    *matches = 1;
    return CUP_OK;
}

CupError wrapper_plan_check(const WrapperPlan *wrappers, size_t *issue_count) {
    ScanContext scan;
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    char binary_path[MAX_PATH_LEN];
    const char *binary_name;
    int names_case_sensitive;
    size_t i;

    if (wrappers == NULL || issue_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *issue_count = 0;

    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    err = wrapper_names_case_sensitive(&names_case_sensitive);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < wrappers->count; ++i) {
        int matches;

        err = compare_wrapper(bin_dir, &wrappers->items[i], &matches);
        if (err != CUP_OK) {
            return err;
        }
        if (!matches) {
            printf("Issue: wrapper '%s' is missing or inconsistent.\n", wrappers->items[i].name);
            (*issue_count)++;
        }
    }

    binary_name = path_last_segment(binary_path);
    if (binary_name == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    scan.wrappers = wrappers;
    scan.binary_name = binary_name;
    scan.names_case_sensitive = names_case_sensitive;
    scan.remove_stale = 0;
    scan.issue_count = issue_count;
    scan.unexpected_found = NULL;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}
