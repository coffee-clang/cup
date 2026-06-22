#include "entrypoints.h"

#include "filesystem.h"
#include "info.h"
#include "layout.h"
#include "package.h"
#include "path.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const EntryPointPlan *entrypoints;
    const char *binary_name;
    int remove_stale;
    size_t *issue_count;
} ScanContext;

void entrypoint_plan_init(EntryPointPlan *plan) {
    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
}

void entrypoint_plan_free(EntryPointPlan *entrypoints) {
    if (entrypoints == NULL) {
        return;
    }

    free(entrypoints->items);
    memset(entrypoints, 0, sizeof(*entrypoints));
}

static int names_equal_case_insensitive(const char *left,
    const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
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

static int find_entrypoint(const EntryPointPlan *entrypoints,
    const char *name) {
    size_t i;

    for (i = 0; i < entrypoints->count; ++i) {
        if (names_equal(entrypoints->items[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static CupError add_entrypoint(EntryPointPlan *entrypoints,
    const char *name, const char *target) {
    EntryPointSpec *items;
    size_t capacity;
    int existing;

    existing = find_entrypoint(entrypoints, name);
    if (existing >= 0) {
        if (strcmp(entrypoints->items[existing].target, target) == 0) {
            return CUP_OK;
        }

        fprintf(stderr,
            "Error: entry point '%s' is declared by more than one default package.\n",
            name);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (entrypoints->count == entrypoints->capacity) {
        capacity = entrypoints->capacity == 0 ? 16 : entrypoints->capacity * 2;
        items = realloc(entrypoints->items, capacity * sizeof(*items));
        if (items == NULL) {
            return CUP_ERR_TEMPORARY;
        }
        entrypoints->items = items;
        entrypoints->capacity = capacity;
    }

    if (text_copy(entrypoints->items[entrypoints->count].name,
            sizeof(entrypoints->items[entrypoints->count].name), name) != CUP_OK ||
        text_copy(entrypoints->items[entrypoints->count].target,
            sizeof(entrypoints->items[entrypoints->count].target), target) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    entrypoints->count++;
    return CUP_OK;
}

static CupError build_entrypoint_name(char *buffer, size_t size,
    const StateEntry *default_entry, const char *entry_name) {
    CupError err;

    if (strcmp(default_entry->host_platform,
            default_entry->target_platform) == 0) {
        err = text_copy(buffer, size, entry_name);
    } else {
        err = text_format(buffer, size, "%s-%s",
            default_entry->target_platform, entry_name);
    }
    if (err != CUP_OK) {
        return err;
    }

#if defined(_WIN32)
    {
        char command_name[MAX_ENTRYPOINT_NAME_LEN];

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

static CupError collect_package_entrypoints(EntryPointPlan *entrypoints,
    const StateEntry *default_entry) {
    PackageIdentity package;
    PackageInfo info;
    const PackageInfoField *field;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char info_path[MAX_PATH_LEN];
    size_t cursor = 0;

    info_init(&info);

    err = package_identity_from_entry(&package, default_entry->component,
        default_entry->host_platform, default_entry->target_platform,
        default_entry->entry);
    if (err != CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path), &package) != CUP_OK ||
        package_validate(install_path, &package) != CUP_OK ||
        path_join(info_path, sizeof(info_path), install_path,
            CUP_INFO_FILENAME) != CUP_OK ||
        info_load(&info, info_path) != CUP_OK) {
        info_free(&info);
        return CUP_ERR_VALIDATION;
    }

    while ((field = info_next(&info, "entry.", &cursor)) != NULL) {
        char name[MAX_ENTRYPOINT_NAME_LEN];
        char target[MAX_PATH_LEN];
        const char *entry_name = field->key + strlen("entry.");

        err = build_entrypoint_name(name, sizeof(name),
            default_entry, entry_name);
        if (err != CUP_OK ||
            text_format(target, sizeof(target),
                "../components/%s/%s/%s/%s/%s/%s",
                package.component, package.tool, package.host_platform,
                package.target_platform, package.version, field->value) != CUP_OK) {
            info_free(&info);
            return CUP_ERR_BUFFER_TOO_SMALL;
        }

        if (strcmp(default_entry->host_platform,
                default_entry->target_platform) == 0 &&
            names_equal_case_insensitive(entry_name, "cup")) {
            fprintf(stderr,
                "Error: package entry point '%s' conflicts with cup itself.\n",
                entry_name);
            info_free(&info);
            return CUP_ERR_INCONSISTENT_STATE;
        }

        err = add_entrypoint(entrypoints, name, target);
        if (err != CUP_OK) {
            info_free(&info);
            return err;
        }
    }

    info_free(&info);
    return CUP_OK;
}

static CupError collect_entrypoints(const CupState *state,
    EntryPointPlan *entrypoints) {
    CupError err;
    char host[MAX_PLATFORM_LEN];
    size_t i;

    if (state == NULL || entrypoints == NULL) {
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

        err = collect_package_entrypoints(entrypoints,
            &state->defaults[i]);
        if (err != CUP_OK) {
            entrypoint_plan_free(entrypoints);
            return err;
        }
    }

    return CUP_OK;
}

static CupError append_text(char *buffer, size_t size, size_t *length,
    const char *text) {
    size_t text_length = strlen(text);

    if (*length + text_length >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    buffer[*length] = '\0';
    return CUP_OK;
}

#if !defined(_WIN32)
static CupError append_shell_quoted(char *buffer, size_t size,
    size_t *length, const char *text) {
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
            char character[2] = {*cursor, '\0'};
            err = append_text(buffer, size, length, character);
        }
        if (err != CUP_OK) {
            return err;
        }
    }

    return append_text(buffer, size, length, "'");
}
#endif

static CupError build_wrapper_content(const EntryPointSpec *entrypoint,
    char **content, size_t *content_size) {
    size_t capacity;
    size_t length = 0;
    char *buffer;
    CupError err;

    if (entrypoint == NULL || content == NULL || content_size == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    capacity = strlen(entrypoint->target) * 4 + 512;
    buffer = calloc(capacity, 1);
    if (buffer == NULL) {
        return CUP_ERR_TEMPORARY;
    }

#if defined(_WIN32)
    {
        const char *cursor;

        err = append_text(buffer, capacity, &length, "@echo off\r\n\"%~dp0");
        if (err == CUP_OK) {
            for (cursor = entrypoint->target; *cursor != '\0'; ++cursor) {
                if (*cursor == '%') {
                    err = append_text(buffer, capacity, &length, "%%");
                } else {
                    char character[2] = {
                        *cursor == '/' ? '\\' : *cursor, '\0'
                    };
                    err = append_text(buffer, capacity, &length, character);
                }
                if (err != CUP_OK) {
                    break;
                }
            }
        }
        if (err == CUP_OK) {
            err = append_text(buffer, capacity, &length,
                "\" %*\r\n");
        }
    }
#else
    err = append_text(buffer, capacity, &length,
        "#!/bin/sh\n"
        "entrypoint_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd) || exit 1\n"
        "exec \"$entrypoint_dir\"/");
    if (err == CUP_OK) {
        err = append_shell_quoted(buffer, capacity, &length,
            entrypoint->target);
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

static CupError write_entrypoint(const char *bin_dir,
    const EntryPointSpec *entrypoint) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    FILE *file = NULL;
    char destination[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    char *content = NULL;
    size_t content_size = 0;

    if (path_join(destination, sizeof(destination), bin_dir,
            entrypoint->name) != CUP_OK ||
        system_create_temp_file(bin_dir, "entrypoint", temporary,
            sizeof(temporary), &file) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = build_wrapper_content(entrypoint, &content, &content_size);
    if (err == CUP_OK &&
        fwrite(content, 1, content_size, file) != content_size) {
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
    return err;
}

static int entrypoint_is_expected(const EntryPointPlan *entrypoints,
    const char *name) {
    return find_entrypoint(entrypoints, name) >= 0;
}

static CupError scan_bin_entry(const char *path, SystemPathKind kind,
    void *userdata) {
    ScanContext *context = userdata;
    const char *name;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL || names_equal(name, context->binary_name) ||
        entrypoint_is_expected(context->entrypoints, name)) {
        return CUP_OK;
    }

    if (context->remove_stale) {
        return kind == SYSTEM_PATH_DIRECTORY
            ? filesystem_remove_tree(path)
            : system_remove_file(path);
    }

    printf("Issue: stale or unmanaged entry point '%s' exists.\n", path);
    (*context->issue_count)++;
    return CUP_OK;
}

static CupError compare_entrypoint(const char *bin_dir,
    const EntryPointSpec *entrypoint, int *matches) {
    CupError err;
    SystemPathKind kind;
    FILE *file;
    char path[MAX_PATH_LEN];
    char *expected = NULL;
    size_t expected_size = 0;
    char *actual = NULL;
    size_t read_size;
    int extra;

    *matches = 0;
    if (path_join(path, sizeof(path), bin_dir, entrypoint->name) != CUP_OK) {
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

    err = build_wrapper_content(entrypoint, &expected, &expected_size);
    if (err != CUP_OK) {
        return err;
    }

    actual = malloc(expected_size == 0 ? 1 : expected_size);
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

    *matches = read_size == expected_size && extra == EOF &&
        memcmp(actual, expected, expected_size) == 0;
    free(actual);
    free(expected);
    return CUP_OK;
}

CupError entrypoint_plan_build_default(EntryPointPlan *plan,
    const StateEntry *default_entry) {
    if (plan == NULL || default_entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    entrypoint_plan_free(plan);
    return collect_package_entrypoints(plan, default_entry);
}

CupError entrypoint_plan_build(EntryPointPlan *plan, const CupState *state) {
    if (plan == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    entrypoint_plan_free(plan);
    return collect_entrypoints(state, plan);
}

CupError entrypoint_plan_apply(const EntryPointPlan *entrypoints) {
    ScanContext scan;
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    char binary_path[MAX_PATH_LEN];
    const char *binary_name;
    size_t i;

    if (entrypoints == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        filesystem_ensure_directory(bin_dir) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < entrypoints->count; ++i) {
        err = write_entrypoint(bin_dir, &entrypoints->items[i]);
        if (err != CUP_OK) {
            return err;
        }
    }

    binary_name = path_last_segment(binary_path);
    if (binary_name == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    scan.entrypoints = entrypoints;
    scan.binary_name = binary_name;
    scan.remove_stale = 1;
    scan.issue_count = NULL;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}

CupError entrypoint_plan_expected_matches(const EntryPointPlan *entrypoints,
    int *matches) {
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    size_t i;

    if (entrypoints == NULL || matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < entrypoints->count; ++i) {
        int entrypoint_matches;

        err = compare_entrypoint(bin_dir, &entrypoints->items[i],
            &entrypoint_matches);
        if (err != CUP_OK) {
            return err;
        }
        if (!entrypoint_matches) {
            return CUP_OK;
        }
    }

    *matches = 1;
    return CUP_OK;
}

CupError entrypoint_plan_check(const EntryPointPlan *entrypoints,
    size_t *issue_count) {
    ScanContext scan;
    CupError err;
    char bin_dir[MAX_PATH_LEN];
    char binary_path[MAX_PATH_LEN];
    const char *binary_name;
    size_t i;

    if (entrypoints == NULL || issue_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *issue_count = 0;

    if (layout_get_bin_dir(bin_dir, sizeof(bin_dir)) != CUP_OK ||
        layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    for (i = 0; i < entrypoints->count; ++i) {
        int matches;

        err = compare_entrypoint(bin_dir, &entrypoints->items[i], &matches);
        if (err != CUP_OK) {
            return err;
        }
        if (!matches) {
            printf("Issue: entry point '%s' is missing or inconsistent.\n",
                entrypoints->items[i].name);
            (*issue_count)++;
        }
    }

    binary_name = path_last_segment(binary_path);
    if (binary_name == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    scan.entrypoints = entrypoints;
    scan.binary_name = binary_name;
    scan.remove_stale = 0;
    scan.issue_count = issue_count;
    return system_list_directory(bin_dir, scan_bin_entry, &scan);
}
