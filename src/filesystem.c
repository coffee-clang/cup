/*
 * Provides portable composite tree operations built on system.h primitives, including recursive
 * removal, directory cleanup and preservation of invalid files.
 */

#include "filesystem.h"

#include "constants.h"
#include "interrupt.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void filesystem_snapshot_init(PersistentFileSnapshot *snapshot) {
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

void filesystem_snapshot_release(PersistentFileSnapshot *snapshot) {
    if (snapshot != NULL) {
        free(snapshot->data);
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

CupError filesystem_snapshot_read(const char *path,
                                  size_t maximum_bytes,
                                  PersistentFileSnapshot *snapshot,
                                  int *missing) {
    FILE *file = NULL;
    SystemPathIdentity identity;
    uint64_t reported_size = 0;
    unsigned char *data = NULL;
    size_t size;
    size_t read_count;
    CupError err;
    int opened_missing = 0;
    int trailing;

    if (text_is_empty(path) || maximum_bytes == 0 || snapshot == NULL || missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    filesystem_snapshot_release(snapshot);
    *missing = 0;
    memset(&identity, 0, sizeof(identity));

    err = system_open_regular_file(
        path, &file, &identity, &reported_size, &opened_missing);
    if (err != CUP_OK || opened_missing) {
        *missing = opened_missing;
        return err;
    }
    if (reported_size > maximum_bytes || reported_size > SIZE_MAX - 1u) {
        return fclose(file) == 0 ? CUP_ERR_BUFFER_TOO_SMALL : CUP_ERR_FILESYSTEM;
    }
    size = (size_t)reported_size;
    data = malloc(size + 1u);
    if (data == NULL) {
        return fclose(file) == 0 ? CUP_ERR_TEMPORARY : CUP_ERR_FILESYSTEM;
    }

    read_count = size == 0 ? 0 : fread(data, 1, size, file);
    trailing = fgetc(file);
    err = read_count == size && trailing == EOF && ferror(file) == 0
              ? CUP_OK
              : CUP_ERR_FILESYSTEM;
    if (fclose(file) != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err != CUP_OK) {
        free(data);
        return err;
    }
    data[size] = '\0';
    snapshot->data = data;
    snapshot->size = size;
    snapshot->identity = identity;
    return CUP_OK;
}

typedef enum {
    FILESYSTEM_PUBLICATION_CREATE,
    FILESYSTEM_PUBLICATION_REPLACE
} FilesystemPublicationMode;

static CupError publish_file(const char *directory,
                             const char *temporary_prefix,
                             const char *destination,
                             const SystemPathIdentity *expected_identity,
                             int executable,
                             FilesystemFileWriter writer,
                             const void *value,
                             FilesystemPublicationMode mode) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    char destination_parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;
    int close_failed;

    if (text_is_empty(directory) || text_is_empty(temporary_prefix) ||
        text_is_empty(destination) || (executable != 0 && executable != 1) || writer == NULL ||
        (mode != FILESYSTEM_PUBLICATION_CREATE && mode != FILESYSTEM_PUBLICATION_REPLACE) ||
        (mode == FILESYSTEM_PUBLICATION_CREATE && expected_identity != NULL) ||
        (expected_identity != NULL &&
         (!expected_identity->valid || expected_identity->kind != SYSTEM_PATH_REGULAR_FILE))) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_parent(destination_parent, sizeof(destination_parent), destination);
    if (err != CUP_OK) {
        return err;
    }
    if (!path_equal(destination_parent, directory)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_create_temp_file(
        directory, temporary_prefix, temporary, sizeof(temporary), &file);
    if (err != CUP_OK) {
        return err;
    }

    err = writer(file, value);
    if (err == CUP_OK && executable) {
        err = system_set_file_executable(file, 1);
    }
    if (err == CUP_OK) {
        err = system_sync_file(file);
    }
    close_failed = fclose(file) != 0;
    file = NULL;
    if (err != CUP_OK || close_failed) {
        (void)system_remove_file(temporary);
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }

    if (mode == FILESYSTEM_PUBLICATION_CREATE) {
        err = system_move_path(temporary, destination, &commit_state);
    } else if (expected_identity != NULL) {
        err = system_replace_file_if_identity(
            temporary, destination, expected_identity, &commit_state);
    } else {
        err = system_replace_file(temporary, destination, &commit_state);
    }
    if (err == CUP_OK && commit_state == SYSTEM_COMMIT_DURABLE) {
        return CUP_OK;
    }
    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        (void)system_remove_file(temporary);
        return err == CUP_OK ? CUP_ERR_FILESYSTEM : err;
    }

    return CUP_ERR_COMMIT;
}

CupError filesystem_publish_new_file(const char *directory,
                                     const char *temporary_prefix,
                                     const char *destination,
                                     int executable,
                                     FilesystemFileWriter writer,
                                     const void *value) {
    return publish_file(directory,
                        temporary_prefix,
                        destination,
                        NULL,
                        executable,
                        writer,
                        value,
                        FILESYSTEM_PUBLICATION_CREATE);
}

CupError filesystem_replace_file_atomically(const char *directory,
                                            const char *temporary_prefix,
                                            const char *destination,
                                            int executable,
                                            FilesystemFileWriter writer,
                                            const void *value) {
    return publish_file(directory,
                        temporary_prefix,
                        destination,
                        NULL,
                        executable,
                        writer,
                        value,
                        FILESYSTEM_PUBLICATION_REPLACE);
}

CupError filesystem_replace_file_if_identity(const char *directory,
                                             const char *temporary_prefix,
                                             const char *destination,
                                             const SystemPathIdentity *expected_identity,
                                             int executable,
                                             FilesystemFileWriter writer,
                                             const void *value) {
    if (expected_identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return publish_file(directory,
                        temporary_prefix,
                        destination,
                        expected_identity,
                        executable,
                        writer,
                        value,
                        FILESYSTEM_PUBLICATION_REPLACE);
}

/* Basic directory creation and recursive removal. */
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

CupError filesystem_remove_tree(const char *path) {
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return system_remove_tree(path, interrupt_requested);
}

CupError filesystem_apply_required_permissions(const char *path, int executable, int read_only) {
    CupError err;

    if (text_is_empty(path) || (executable != 0 && executable != 1) ||
        (read_only != 0 && read_only != 1)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_set_executable(path, executable);
    if (err != CUP_OK) {
        return err;
    }
    return system_set_read_only(path, read_only);
}

/* Nonrecursive directory inspection used by doctor and cleanup. */
typedef struct {
    const char *excluded_path;
    size_t child_count;
} CountContext;

static CupError count_child(const char *path,
                            SystemPathKind path_kind,
                            const SystemPathIdentity *identity,
                            void *userdata) {
    CountContext *context = userdata;

    if (identity == NULL || !identity->valid || identity->kind != path_kind) {
        return CUP_ERR_FILESYSTEM;
    }
    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!text_is_empty(context->excluded_path) && path_equal(path, context->excluded_path)) {
        return CUP_OK;
    }

    context->child_count++;
    return CUP_OK;
}

CupError filesystem_count_children(const char *path,
                                   const char *excluded_path,
                                   size_t *child_count) {
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
                                      SystemPathKind path_kind,
                                      const SystemPathIdentity *identity,
                                      void *userdata) {
    ClearContext *context = userdata;

    (void)identity;
    (void)path_kind;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!text_is_empty(context->preserved_path) && path_equal(path, context->preserved_path)) {
        return CUP_OK;
    }

    return system_remove_path_if_identity(path, identity, interrupt_requested);
}

CupError filesystem_clear_directory(const char *path, const char *preserved_path) {
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

/* Preserve invalid evidence under a unique sibling name before repair continues. */
static CupError backup_invalid(const char *path,
                               const SystemPathIdentity *expected_identity,
                               char *backup_path,
                               size_t backup_size) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    SystemPathKind path_kind;
    CupError err;
    char source[MAX_PATH_LEN];
    char candidate[MAX_PATH_LEN];
    int path_exists;
    int restore_read_only = 0;
    unsigned int suffix = 0;

    if (text_is_empty(path) || backup_path == NULL || backup_size == 0 ||
        (expected_identity != NULL &&
         (!expected_identity->valid ||
          (expected_identity->kind != SYSTEM_PATH_REGULAR_FILE &&
           expected_identity->kind != SYSTEM_PATH_DIRECTORY)))) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = text_copy(source, sizeof(source), path);
    if (err != CUP_OK) {
        return err;
    }
    backup_path[0] = '\0';

    err = system_get_path_kind(source, &path_kind);
    if (err != CUP_OK || path_kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (expected_identity != NULL) {
        SystemPathIdentity current_identity;

        err = system_get_path_identity(source, &current_identity);
        if (err != CUP_OK || !system_path_identity_equal(&current_identity, expected_identity)) {
            return CUP_ERR_TRANSACTION;
        }
    }

    do {
        if (suffix == 0) {
            err = text_format(candidate, sizeof(candidate), "%s.invalid", source);
        } else {
            err = text_format(candidate, sizeof(candidate), "%s.invalid.%u", source, suffix);
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

    /* Reject an undersized result buffer before changing the filesystem. */
    if (strlen(candidate) >= backup_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (path_kind == SYSTEM_PATH_REGULAR_FILE) {
        err = system_is_read_only(source, &restore_read_only);
        if (err != CUP_OK) {
            return err;
        }
        if (restore_read_only && system_set_read_only(source, 0) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }
    }

    err = expected_identity == NULL
              ? system_move_path(source, candidate, &commit_state)
              : system_move_path_if_identity(
                    source, candidate, expected_identity, &commit_state);
    if (err == CUP_OK && commit_state != SYSTEM_COMMIT_DURABLE) {
        err = CUP_ERR_COMMIT;
    }
    if (err != CUP_OK) {
        if (commit_state != SYSTEM_COMMIT_NOT_APPLIED) {
            if (restore_read_only) {
                (void)system_set_read_only(candidate, 1);
            }
            return CUP_ERR_COMMIT;
        }

        if (restore_read_only && system_set_read_only(source, 1) != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        return err;
    }

    /* A repair backup preserves the original write-protection class as evidence. */
    if (restore_read_only && system_set_read_only(candidate, 1) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }

    memcpy(backup_path, candidate, strlen(candidate) + 1);
    return CUP_OK;
}

CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size) {
    return backup_invalid(path, NULL, backup_path, backup_size);
}

CupError filesystem_backup_invalid_if_identity(const char *path,
                                               const SystemPathIdentity *expected_identity,
                                               char *backup_path,
                                               size_t backup_size) {
    if (expected_identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return backup_invalid(path, expected_identity, backup_path, backup_size);
}
