#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"

typedef enum {
    SYSTEM_PATH_MISSING,
    SYSTEM_PATH_REGULAR_FILE,
    SYSTEM_PATH_DIRECTORY,
    SYSTEM_PATH_LINK,
    SYSTEM_PATH_OTHER
} SystemPathKind;

typedef enum {
    SYSTEM_COMMIT_NOT_APPLIED,
    SYSTEM_COMMIT_APPLIED,
    SYSTEM_COMMIT_DURABLE
} SystemCommitState;

typedef CupError (*SystemDirectoryCallback)(const char *path,
    SystemPathKind kind, void *userdata);

typedef enum {
    SYSTEM_LOCK_SHARED,
    SYSTEM_LOCK_EXCLUSIVE
} SystemLockMode;

typedef struct {
    intptr_t handle;
    int active;
} SystemLock;

/* Process and user environment. */
CupError system_get_home_dir(char *buffer, size_t size);
unsigned long system_get_process_id(void);
CupError system_start_uninstall(const char *cup_root,
    const char *uninstall_script, unsigned long parent_pid);
CupError system_start_self_update(const char *staging_directory,
    const char *installed_binary, const char *installed_uninstall,
    const char *installed_checksums, const char *lock_path,
    const char *journal_path, unsigned long parent_pid);

/* Files and directories. */
CupError system_make_directory(const char *path);
CupError system_remove_directory(const char *path);
CupError system_move_path(const char *source, const char *destination,
    SystemCommitState *commit_state);
CupError system_replace_file(const char *source, const char *destination,
    SystemCommitState *commit_state);
CupError system_remove_file(const char *path);
CupError system_copy_file(const char *source_path, const char *destination_path);
CupError system_sync_file(FILE *file);
CupError system_sync_parent_directory(const char *path);

/* Exclusive temporary objects created below a caller-selected directory. */
CupError system_create_file_exclusive(const char *path, FILE **file);
CupError system_create_temp_file(const char *directory, const char *prefix,
    char *path, size_t path_size, FILE **file);
CupError system_create_temp_directory(const char *directory, const char *prefix,
    char *path, size_t path_size);
CupError system_make_unique_temp_path(const char *directory, const char *prefix,
    char *path, size_t path_size);

/* Path inspection and permissions. Inspection never follows links. */
CupError system_get_path_kind(const char *path, SystemPathKind *kind);
CupError system_path_exists(const char *path, int *exists);
CupError system_is_directory(const char *path, int *is_directory);
CupError system_is_regular_file(const char *path, int *is_regular_file);
CupError system_file_size(const char *path, long long *file_size);
CupError system_is_read_only(const char *path, int *is_read_only);
CupError system_is_executable(const char *path, int *is_executable);
CupError system_set_read_only(const char *path, int read_only);
CupError system_set_executable(const char *path, int executable);

/* Directory traversal. */
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata);
CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata);

/* Process-scoped operating-system locks. */
CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode);
void system_lock_release(SystemLock *lock);

#endif /* CUP_SYSTEM_H */
