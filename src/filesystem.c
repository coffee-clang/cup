#include "filesystem.h"

#include "constants.h"
#include "interrupt.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// DIRECTORY CREATION AND REMOVAL
CupError filesystem_ensure_directory(const char *path) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        err = system_is_directory(path, &is_directory);
        if (err != CUP_OK || !is_directory) {
            fprintf(stderr, "Error: '%s' exists but is not a directory.\n", path);
            return CUP_ERR_FILESYSTEM;
        }
        return CUP_OK;
    }

    return system_make_directory(path);
}

static CupError remove_tree_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    (void)userdata;

    if (is_empty_string(path) || info == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }
    if (info->is_directory && !info->is_reparse_point) {
        return system_remove_directory(path);
    }
    return system_remove_file(path);
}

CupError filesystem_remove_tree(const char *path) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }
    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_directory) {
        return system_remove_file(path);
    }

    err = system_walk_directory(path, remove_tree_entry, NULL);
    if (err != CUP_OK) {
        return err;
    }
    return system_remove_directory(path);
}

// DIRECTORY CONTENTS
typedef struct {
    const char *excluded_path;
    size_t count;
} CountContext;

static CupError count_child_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    CountContext *context = userdata;
    (void)info;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!is_empty_string(context->excluded_path) && strcmp(path, context->excluded_path) == 0) {
        return CUP_OK;
    }
    context->count++;
    return CUP_OK;
}

CupError filesystem_count_children(const char *path, const char *excluded_path, size_t *count) {
    CountContext context;
    CupError err;
    int exists;

    if (is_empty_string(path) || count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *count = 0;
    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }
    context.excluded_path = excluded_path;
    context.count = 0;
    err = system_list_directory(path, count_child_entry, &context);
    if (err != CUP_OK) {
        return err;
    }
    *count = context.count;
    return CUP_OK;
}

typedef struct {
    const char *preserved_path;
} ClearContext;

static CupError clear_directory_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    ClearContext *context = userdata;
    (void)info;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!is_empty_string(context->preserved_path) && strcmp(path, context->preserved_path) == 0) {
        return CUP_OK;
    }
    return filesystem_remove_tree(path);
}

CupError filesystem_clear_directory(const char *path, const char *preserved_path) {
    ClearContext context;
    int exists;
    CupError err;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }
    context.preserved_path = preserved_path;
    return system_list_directory(path, clear_directory_entry, &context);
}

// INVALID FILE BACKUPS
CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size) {
    CupError err;
    char candidate[MAX_PATH_LEN];
    int exists;
    unsigned int index;

    if (is_empty_string(path) || backup_path == NULL || backup_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }

    index = 0;
    while (1) {
        if (index == 0) {
            err = checked_snprintf(candidate, sizeof(candidate), "%s.invalid", path);
        } else {
            err = checked_snprintf(candidate, sizeof(candidate), "%s.invalid.%u", path, index);
        }
        if (err != CUP_OK) {
            return err;
        }

        err = system_path_exists(candidate, &exists);
        if (err != CUP_OK) {
            return err;
        }
        if (!exists) {
            break;
        }
        index++;
    }

    system_set_read_only(path, 0);
    err = system_move_path(path, candidate);
    if (err != CUP_OK) {
        return err;
    }
    return checked_snprintf(backup_path, backup_size, "%s", candidate);
}
