#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"

typedef struct {
    int is_directory;
    int is_reparse_point;
    int is_regular_file;
} SystemPathInfo;

typedef CupError (*SystemDirectoryCallback)( const char *path, const SystemPathInfo *info, void *userdata);

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
CupError system_get_process_id(char *buffer, size_t size);
CupError system_start_uninstall(const char *cup_root, const char *uninstall_script);

/* Files and directories. */
CupError system_make_directory(const char *path);
CupError system_remove_directory(const char *path);
CupError system_move_path(const char *source, const char *destination);
CupError system_replace_file(const char *source, const char *destination);
CupError system_remove_file(const char *path);
CupError system_copy_file(const char *source_path, const char *destination_path);
CupError system_sync_file(FILE *file);

/* Path inspection and permissions. */
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
