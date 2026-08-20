#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

/*
 * Portable operating-system contract for paths, permissions, locks, durable replacement and
 * detached helpers. Higher layers own policy; this layer reports what the OS actually applied.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"

/* Path type of the final entry. A final symbolic link or reparse point is classified as a link
 * rather than followed; APIs that require trusted traversal pin parent components separately. */
typedef enum {
    SYSTEM_PATH_MISSING,
    SYSTEM_PATH_REGULAR_FILE,
    SYSTEM_PATH_DIRECTORY,
    SYSTEM_PATH_LINK,
    SYSTEM_PATH_OTHER
} SystemPathKind;

/* Native identity snapshot used to detect pathname replacement across a command. `object` is
 * the low 64 bits; platforms with wider native IDs store the upper 64 bits in `object_high`. A
 * backend may refresh this snapshot after an operation when its filesystem can change file IDs. */
typedef struct {
    uint64_t volume;
    uint64_t object;
    uint64_t object_high;
    SystemPathKind kind;
    int valid;
} SystemPathIdentity;

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

typedef CupError (*SystemDirectoryCallback)(const char *path,
                                            SystemPathKind kind,
                                            const SystemPathIdentity *identity,
                                            void *userdata);

typedef enum {
    SYSTEM_LOCK_SHARED,
    SYSTEM_LOCK_EXCLUSIVE
} SystemLockMode;

/* Lock handle. Initialize storage to zero before first acquisition and never reacquire while
 * active. */
typedef struct {
    intptr_t handle;
    int active;
} SystemLock;

/* Process and user environment. */
void system_set_restrictive_umask(void);
CupError system_get_home_dir(char *buffer, size_t size);
unsigned long system_get_process_id(void);

/*
 * Start detached helpers that must outlive the current cup process. All paths have already been
 * validated by the calling command. Uninstall must own lock_path before acknowledging the handoff
 * and retain that authority until the canonical root is detached, so repair cannot race the
 * pre-detach helper protocol.
 */
CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                const char *detached_root,
                                const char *lock_path);
CupError system_start_cup_update_helper(const char *helper, const char *token);
CupError system_wait_for_parent_exit(const char *wait_value);
CupError system_sleep_milliseconds(unsigned int milliseconds);

/* Single-path filesystem operations. */
CupError system_make_directory(const char *path);
/* Validate or create every directory component without following links. */
CupError system_check_directory_chain(const char *path, int allow_missing);
CupError system_make_directory_chain(const char *path);
/* Create exactly one final directory and report its durability boundary. */
CupError system_create_directory_exclusive(const char *path,
                                           unsigned int mode,
                                           SystemCommitState *commit_state);
CupError system_create_private_directory(const char *path,
                                         SystemCommitState *commit_state);
CupError system_make_private_directory(const char *path);
CupError system_directory_is_private(const char *path, int *is_private);
CupError system_remove_directory(const char *path);

/*
 * Move or replace one path and report whether the destination remained untouched, became
 * visible with uncertain durability, or was committed durably.
 */
CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state);
/* Move only the exact regular-file or directory source observed by the caller. */
CupError system_move_path_if_identity(const char *source,
                                      const char *destination,
                                      const SystemPathIdentity *expected_identity,
                                      SystemCommitState *commit_state);
CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state);
/* Identity-bound replacement requires an observed regular-file destination. */
CupError system_replace_file_if_identity(const char *source,
                                         const char *destination,
                                         const SystemPathIdentity *expected_identity,
                                         SystemCommitState *commit_state);

CupError system_remove_file(const char *path);
/* Identity-bound single-file removal accepts an observed regular file; use the generic
 * path-removal API below when the observed object may instead be a link or directory. */
CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity);
/* Remove exactly one previously observed path, recursively when it is a directory. */
CupError system_remove_path_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity,
                                        int (*cancelled)(void));
/* Recursively remove one tree without following links or reparse points. */
CupError system_remove_tree(const char *path, int (*cancelled)(void));
CupError system_remove_tree_contents(const char *path,
                                     const char *preserve_name,
                                     int (*cancelled)(void));
/* Copy through a sibling temporary file so a failed copy preserves destination. */
CupError system_copy_file(const char *source_path, const char *destination_path);
/* Apply the executable class to an open regular file before its final durability sync. */
CupError system_set_file_executable(FILE *file, int executable);
/* Read the executable class from the already-open regular file. The path is used only for
 * filename-based platform policy such as Windows command extensions. */
CupError system_file_is_executable(FILE *file, const char *path, int *is_executable);
CupError system_sync_file(FILE *file);
CupError system_sync_parent_directory(const char *path);

/* Exclusive temporary objects created below a caller-selected directory. `prefix` must be one
 * safe path segment so it cannot change that directory selection. */
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

/* Open one regular file without following links. Missing is reported separately. */
CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing);

/* Path inspection and permissions. Inspection does not follow a final link; mutations that
 * require trusted traversal additionally reject link/reparse-point parent components. */
CupError system_get_path_kind(const char *path, SystemPathKind *kind);
CupError system_get_path_identity(const char *path, SystemPathIdentity *identity);
int system_path_identity_equal(const SystemPathIdentity *left,
                               const SystemPathIdentity *right);
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

/* Acquire and release a nonblocking advisory lock owned by the SystemLock handle. Exclusive
 * acquisition may create the lock file; shared acquisition is read-only and requires the file to
 * exist. */
CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode);
CupError system_lock_acquire_existing(SystemLock *lock,
                                      const char *path,
                                      SystemLockMode mode);
CupError system_lock_get_identity(const SystemLock *lock, SystemPathIdentity *identity);
CupError system_lock_read(const SystemLock *lock,
                          void *buffer,
                          size_t capacity,
                          size_t *size);
void system_lock_release(SystemLock *lock);

#endif /* CUP_SYSTEM_H */
