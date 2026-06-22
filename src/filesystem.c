#include "filesystem.h"

#include "constants.h"
#include "interrupt.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

CupError filesystem_ensure_directory(const char *path) {
    SystemPathKind path_kind;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(path, &path_kind);
    if (err != CUP_OK) {
        return err;
    }

    if (path_kind == SYSTEM_PATH_MISSING) {
        return system_make_directory(path);
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        fprintf(stderr, "Error: '%s' exists but is not a directory.\n", path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError remove_tree_entry(const char *path,
    SystemPathKind path_kind, void *userdata) {
    (void)userdata;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (path_kind == SYSTEM_PATH_DIRECTORY) {
        return system_remove_directory(path);
    }

    return system_remove_file(path);
}

CupError filesystem_remove_tree(const char *path) {
    SystemPathKind path_kind;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(path, &path_kind);
    if (err != CUP_OK || path_kind == SYSTEM_PATH_MISSING) {
        return err;
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        return system_remove_file(path);
    }

    err = system_walk_directory(path, remove_tree_entry, NULL);
    if (err != CUP_OK) {
        return err;
    }

    return system_remove_directory(path);
}

typedef struct {
    const char *excluded_path;
    size_t child_count;
} CountContext;

static CupError count_child(const char *path,
    SystemPathKind path_kind, void *userdata) {
    CountContext *context = userdata;
    (void)path_kind;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!text_is_empty(context->excluded_path) &&
        strcmp(path, context->excluded_path) == 0) {
        return CUP_OK;
    }

    context->child_count++;
    return CUP_OK;
}

CupError filesystem_count_children(const char *path,
    const char *excluded_path, size_t *child_count) {
    CountContext context;
    SystemPathKind path_kind;
    CupError err;

    if (text_is_empty(path) || child_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *child_count = 0;
    err = system_get_path_kind(path, &path_kind);
    if (err != CUP_OK || path_kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }

    context.excluded_path = excluded_path;
    context.child_count = 0;
    err = system_list_directory(path, count_child, &context);
    if (err != CUP_OK) {
        return err;
    }

    *child_count = context.child_count;
    return CUP_OK;
}

typedef struct {
    const char *preserved_path;
} ClearContext;

static CupError clear_directory_entry(const char *path,
    SystemPathKind path_kind, void *userdata) {
    ClearContext *context = userdata;
    (void)path_kind;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!text_is_empty(context->preserved_path) &&
        strcmp(path, context->preserved_path) == 0) {
        return CUP_OK;
    }

    return filesystem_remove_tree(path);
}

CupError filesystem_clear_directory(const char *path,
    const char *preserved_path) {
    ClearContext context;
    SystemPathKind path_kind;
    CupError err;

    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(path, &path_kind);
    if (err != CUP_OK || path_kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }

    context.preserved_path = preserved_path;
    return system_list_directory(path, clear_directory_entry, &context);
}

CupError filesystem_backup_invalid(const char *path,
    char *backup_path, size_t backup_size) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    SystemPathKind path_kind;
    CupError err;
    char candidate[MAX_PATH_LEN];
    int path_exists;
    int restore_read_only = 0;
    unsigned int suffix = 0;

    if (text_is_empty(path) || backup_path == NULL || backup_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(path, &path_kind);
    if (err != CUP_OK || path_kind == SYSTEM_PATH_MISSING) {
        return err;
    }

    do {
        if (suffix == 0) {
            err = text_format(candidate, sizeof(candidate),
                "%s.invalid", path);
        } else {
            err = text_format(candidate, sizeof(candidate),
                "%s.invalid.%u", path, suffix);
        }
        if (err != CUP_OK) {
            return err;
        }

        err = system_path_exists(candidate, &path_exists);
        if (err != CUP_OK) {
            return err;
        }
        suffix++;
    } while (path_exists);

    if (path_kind == SYSTEM_PATH_REGULAR_FILE) {
        err = system_is_read_only(path, &restore_read_only);
        if (err != CUP_OK) {
            return err;
        }
        if (restore_read_only &&
            system_set_read_only(path, 0) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }
    }

    err = system_move_path(path, candidate, &commit_state);
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_APPLIED) {
            return CUP_ERR_COMMIT;
        }

        if (restore_read_only &&
            system_set_read_only(path, 1) != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        return err;
    }

    return text_copy(backup_path, backup_size, candidate);
}
