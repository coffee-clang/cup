#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

/*
 * Module contract: Portable operating-system contract for paths,
 * permissions, locks, durable replacement, and detached helpers. Higher
 * layers own package and transaction policy; this layer reports what the
 * operating system actually applied.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"

/* Path type observed without following a symbolic link or reparse point. */
typedef enum {
    SYSTEM_PATH_MISSING,
    SYSTEM_PATH_REGULAR_FILE,
    SYSTEM_PATH_DIRECTORY,
    SYSTEM_PATH_LINK,
    SYSTEM_PATH_OTHER
} SystemPathKind;

/*
 * Result boundary for move and replace operations.
 *
 * NOT_APPLIED means the destination was not changed. APPLIED means the new
 * destination may already be visible but parent-directory durability could
 * not be confirmed. DURABLE means both replacement and required persistence
 * completed. Callers must not blindly roll back an APPLIED operation.
 */
typedef enum {
    SYSTEM_COMMIT_NOT_APPLIED,
    SYSTEM_COMMIT_APPLIED,
    SYSTEM_COMMIT_DURABLE
} SystemCommitState;

typedef CupError (*SystemDirectoryCallback)(const char *path, SystemPathKind kind, void *userdata);

typedef enum {
    SYSTEM_LOCK_SHARED,
    SYSTEM_LOCK_EXCLUSIVE
} SystemLockMode;

/* Process-owned handle. Initialize storage to zero before first acquisition. */
typedef struct {
    intptr_t handle;
    int active;
} SystemLock;

/* Process and user environment. */
void system_set_restrictive_umask(void);
CupError system_get_home_dir(char *buffer, size_t size);
unsigned long system_get_process_id(void);

/*
 * Start detached helpers that must outlive the current cup process. All paths
 * have already been validated by the calling command.
 */
CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                unsigned long parent_pid);

/* Single-path filesystem operations. */
CupError system_make_directory(const char *path);
CupError system_make_private_directory(const char *path);
CupError system_directory_is_private(const char *path, int *is_private);
CupError system_remove_directory(const char *path);

/*
 * Move or replace one path and report whether the destination was not changed,
 * changed with uncertain durability, or changed durably.
 */
CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state);
CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state);

CupError system_remove_file(const char *path);
/* Recursively remove one tree without following links or reparse points. */
CupError system_remove_tree(const char *path, int (*cancelled)(void));
/* Copy through a sibling temporary file so a failed copy preserves destination. */
CupError system_copy_file(const char *source_path, const char *destination_path);
CupError system_sync_file(FILE *file);
CupError system_sync_parent_directory(const char *path);

/* Exclusive temporary objects created below a caller-selected directory. */
CupError system_create_file_exclusive(const char *path, FILE **file);
CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file);
CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size);
CupError system_make_unique_temp_path(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size);

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

/* List direct children or recursively walk a tree without following links. */
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata);
CupError system_walk_directory(const char *path, SystemDirectoryCallback callback, void *userdata);

/* Acquire and release a process-scoped lock on the canonical lock path. */
CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode);
void system_lock_release(SystemLock *lock);

#endif /* CUP_SYSTEM_H */
