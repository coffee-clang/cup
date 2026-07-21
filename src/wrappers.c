/*
 * Builds immutable managed-wrapper plans from validated active packages and applies or diagnoses
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
    int remove_stale;
    size_t *issue_count;
} ScanContext;

/* Plan lifetime and validated wrapper collection. */
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

static int names_equal(const char *left, const char *right) {
#if defined(_WIN32) || defined(__APPLE__)
    return names_equal_case_insensitive(left, right);
#else
    return strcmp(left, right) == 0;
#endif
}

static int find_wrapper(const WrapperPlan *wrappers, const char *name) {
    size_t i;

    for (i = 0; i < wrappers->count; ++i) {
        if (names_equal(wrappers->items[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static CupError add_wrapper(WrapperPlan *wrappers, const char *name, const char *target) {
    WrapperSpec *items;
    size_t capacity;
    int existing;

    existing = find_wrapper(wrappers, name);
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

/* Public-name selection. Native and target-prefixed names are checked for platform-specific case
 * collisions. */
static CupError build_wrapper_name(char *buffer,
                                   size_t size,
                                   const PackageIdentity *active_identity,
                                   const char *entry_name) {
    CupError err;

    if (strcmp(active_identity->host_platform, active_identity->target_platform) == 0) {
        err = text_copy(buffer, size, entry_name);
    } else {
        err = text_format(buffer, size, "%s-%s", active_identity->target_platform, entry_name);
    }
    if (err != CUP_OK) {
        return err;
    }

#if defined(_WIN32)
    {
        char command_name[MAX_COMMAND_NAME_LEN];

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
                                         const PackageIdentity *active_identity) {
    PackageMetadata metadata;
    PackageCommand command;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char package_metadata_path[MAX_PATH_LEN];
    size_t cursor = 0;

    package_metadata_init(&metadata);

    err = package_identity_validate(active_identity);
    if (err != CUP_OK) {
        goto done;
    }
    err = layout_build_install_path(install_path, sizeof(install_path), active_identity);
    if (err != CUP_OK) {
        goto done;
    }
    err = package_validate(install_path, active_identity);
    if (err != CUP_OK) {
        goto done;
    }
    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), install_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        goto done;
    }
    err = package_metadata_load(&metadata, package_metadata_path);
    if (err != CUP_OK) {
        goto done;
    }

    while (package_metadata_next_command(&metadata, &command, &cursor)) {
        char name[MAX_COMMAND_NAME_LEN];
        char target[MAX_PATH_LEN];

        err = build_wrapper_name(name, sizeof(name), active_identity, command.name);
        if (err != CUP_OK) {
            goto done;
        }
        err = text_format(target,
                          sizeof(target),
                          "../components/%s/%s/%s/%s/%s/%s",
                          active_identity->component,
                          active_identity->tool,
                          active_identity->host_platform,
                          active_identity->target_platform,
                          active_identity->version,
                          command.path);
        if (err != CUP_OK) {
            goto done;
        }

        if (strcmp(active_identity->host_platform, active_identity->target_platform) == 0 &&
            names_equal_case_insensitive(command.name, "cup")) {
            fprintf(
                stderr, "Error: package command '%s' conflicts with cup itself.\n", command.name);
            err = CUP_ERR_INCONSISTENT_STATE;
            goto done;
        }

        err = add_wrapper(wrappers, name, target);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = CUP_OK;

done:
    package_metadata_free(&metadata);
    return err;
}

static CupError collect_wrappers(const CupState *state, WrapperPlan *wrappers) {
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

    for (i = 0; i < state->active_count; ++i) {
        if (strcmp(state->active[i].host_platform, host) != 0) {
            continue;
        }

        err = collect_package_commands(wrappers, &state->active[i]);
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

        err = append_text(buffer, capacity, &length, "@echo off\r\n\"%~dp0");
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

static CupError write_wrapper(const char *bin_dir, const WrapperSpec *wrapper) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    FILE *file = NULL;
    char destination[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    char *content = NULL;
    size_t content_size = 0;

    if (text_is_empty(bin_dir) || wrapper == NULL || text_is_empty(wrapper->name) ||
        text_is_empty(wrapper->target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (path_join(destination, sizeof(destination), bin_dir, wrapper->name) != CUP_OK ||
        system_create_temp_file(bin_dir, "wrapper", temporary, sizeof(temporary), &file) !=
            CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = build_wrapper_content(wrapper, &content, &content_size);
    if (err == CUP_OK && fwrite(content, 1, content_size, file) != content_size) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err == CUP_OK) {
        err = system_sync_file(file);
    }
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    file = NULL;
    free(content);
    if (err != CUP_OK) {
        system_remove_file(temporary);
        return err;
    }

#if !defined(_WIN32)
    err = system_set_executable(temporary, 1);
    if (err != CUP_OK) {
        system_remove_file(temporary);
        return err;
    }
#endif

    err = system_replace_file(temporary, destination, &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

static int wrapper_is_expected(const WrapperPlan *wrappers, const char *name) {
    return find_wrapper(wrappers, name) >= 0;
}

/* Installed-wrapper inspection. Unexpected, stale or modified files are reported without becoming a
 * new source of truth. */
static CupError scan_bin_entry(const char *path, SystemPathKind kind, void *userdata) {
    ScanContext *context = userdata;
    const char *name;

    if (text_is_empty(path) || context == NULL || context->wrappers == NULL ||
        text_is_empty(context->binary_name) ||
        (!context->remove_stale && context->issue_count == NULL)) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL || names_equal(name, context->binary_name) ||
        wrapper_is_expected(context->wrappers, name)) {
        return CUP_OK;
    }

    if (context->remove_stale) {
        return kind == SYSTEM_PATH_DIRECTORY ? filesystem_remove_tree(path)
                                             : system_remove_file(path);
    }

    printf("Issue: stale or unmanaged wrapper '%s' exists.\n", path);
    (*context->issue_count)++;
    return CUP_OK;
}

static CupError compare_wrapper(const char *bin_dir, const WrapperSpec *wrapper, int *matches) {
    CupError err;
    SystemPathKind kind;
    FILE *file;
    char path[MAX_PATH_LEN];
    char *expected = NULL;
    size_t expected_size = 0;
    char *actual = NULL;
    size_t read_size;
    int extra;

    if (text_is_empty(bin_dir) || wrapper == NULL || matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *matches = 0;
    if (path_join(path, sizeof(path), bin_dir, wrapper->name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
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
            return CUP_OK;
        }
    }
#endif

    err = build_wrapper_content(wrapper, &expected, &expected_size);
    if (err != CUP_OK || expected == NULL || expected_size == 0) {
        free(expected);
        return err != CUP_OK ? err : CUP_ERR_INCONSISTENT_STATE;
    }

    actual = malloc(expected_size);
    if (actual == NULL) {
        free(expected);
        return CUP_ERR_TEMPORARY;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        free(actual);
        free(expected);
        return CUP_ERR_FILESYSTEM;
    }

    read_size = fread(actual, 1, expected_size, file);
    extra = read_size == expected_size ? fgetc(file) : EOF;
    {
        int read_failed = ferror(file) != 0;
        int close_failed = fclose(file) != 0;

        if (read_failed || close_failed) {
            free(actual);
            free(expected);
            return CUP_ERR_FILESYSTEM;
        }
    }

    *matches =
        read_size == expected_size && extra == EOF && memcmp(actual, expected, expected_size) == 0;
    free(actual);
    free(expected);
    return CUP_OK;
}

/* Public plan construction, application and diagnosis. */
CupError wrapper_plan_build_active(WrapperPlan *plan, const PackageIdentity *active_identity) {
    if (plan == NULL || active_identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    wrapper_plan_free(plan);
    return collect_package_commands(plan, active_identity);
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    if (plan == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    wrapper_plan_free(plan);
    return collect_wrappers(state, plan);
}

/* Application and diagnostics. The complete plan is written deterministically, then compared by
 * doctor when requested. */
CupError wrapper_plan_apply(const WrapperPlan *wrappers) {
    ScanContext scan;
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    char binary_path[MAX_PATH_LEN];
    const char *binary_name;
    size_t i;

    if (wrappers == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        filesystem_ensure_directory(bin_dir) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < wrappers->count; ++i) {
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
    scan.remove_stale = 1;
    scan.issue_count = NULL;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}

CupError wrapper_plan_expected_matches(const WrapperPlan *wrappers, int *matches) {
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
    size_t i;

    if (wrappers == NULL || issue_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *issue_count = 0;

    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
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
    scan.remove_stale = 0;
    scan.issue_count = issue_count;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}
