#ifndef CUP_SYSTEM_H
#define CUP_SYSTEM_H

/* Native OS boundary for paths, identity, permissions, locks, durable publication and helpers.
 * Higher layers own policy; this layer reports what the OS applied. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"

/* Type of the final entry without following a final symlink/reparse point. */
typedef enum {
    SYSTEM_PATH_MISSING,
    SYSTEM_PATH_REGULAR_FILE,
    SYSTEM_PATH_DIRECTORY,
    SYSTEM_PATH_LINK,
    SYSTEM_PATH_OTHER
} SystemPathKind;

/* Native object identity used to detect pathname replacement. Wider object IDs use
 * `object_high`; backends may refresh identity after operations that can change it. */
typedef struct {
    uint64_t volume;
    uint64_t object;
    uint64_t object_high;
    SystemPathKind kind;
    int valid;
} SystemPathIdentity;

/* Publication state: NOT_APPLIED changed nothing; APPLIED may be visible without durable
 * confirmation; DURABLE completed persistence. Do not blindly roll back APPLIED. */
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

/* Active lock plus acquired mode, retained so helper handoff can prove exclusive authority.
 * Zero-initialize before first acquisition; do not reacquire while active. */
typedef struct {
    intptr_t handle;
    SystemLockMode mode;
    int active;
} SystemLock;

/* Detached-helper authority. POSIX transfers the flock description; Windows uses a root-scoped
 * kernel object because LockFileEx ownership cannot transfer. */
typedef struct {
    intptr_t handle;
    int active;
} SystemHandoff;

/* Process and user environment. */
void system_set_restrictive_umask(void);
CupError system_get_home_dir(char *buffer, size_t size);
unsigned long system_get_process_id(void);

/* Start a detached helper under the canonical exclusive lock. Success transfers authority
 * without an admission gap; Windows uninstall also transfers its pre-armed cleanup handle. */
CupError system_start_update_helper(const char *helper,
                                    const char *root,
                                    const char *token,
                                    SystemLock *lock);
CupError system_start_uninstall_helper(const char *helper,
                                       const char *root,
                                       const char *detached_root,
                                       const char *token,
                                       SystemLock *lock);
#if defined(_WIN32)
/* Prove the inherited DELETE_ON_CLOSE handle names this running uninstall helper before
 * accepting handoff; the parent-side carrier owns cleanup after termination. */
CupError system_validate_uninstall_helper_cleanup(const char *cleanup_handle_value);
#endif
CupError system_handoff_accept(SystemHandoff *handoff,
                               const char *parent_signal_value,
                               const char *authority_value);
/* Return from temporary handoff authority to the canonical lock without an authority gap. */
CupError system_handoff_acquire_lock(SystemHandoff *handoff,
                                     SystemLock *lock,
                                     const char *lock_path);
void system_handoff_release(SystemHandoff *handoff);
/* Report backend-specific external handoff authority that must block root admission. */
CupError system_handoff_active(const char *root, int *active);

/* Resolve the running executable. POSIX may unlink its exact running helper path; Windows uses
 * deferred DELETE_ON_CLOSE cleanup for mapped helper images. */
CupError system_get_executable_path(char *buffer, size_t size);
#if !defined(_WIN32)
CupError system_unlink_running_executable(const char *path);
#endif

/* Single-path filesystem operations. */
CupError system_make_directory(const char *path);
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
/* Same identity-bound move with a bounded retry only for platform-defined transient sharing
 * conflicts. Used when an external process may briefly retain a harmless handle. */
CupError system_move_path_retry(const char *source,
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

/* Open one regular file without following links. Missing is reported separately. On
 * CUP_OK with missing == 0, file is non-NULL and identity is a valid regular-file identity. */
CupError system_open_regular_file(const char *path,
                                  FILE **file,
                                  SystemPathIdentity *identity,
                                  uint64_t *file_size,
                                  int *missing);
/* Open a regular file below a package root. POSIX may resolve package-owned symlinks only when
 * the final object remains beneath the root; Windows keeps no-reparse traversal. */
CupError system_open_regular_file_beneath(const char *root,
                                          const char *relative_path,
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

/* List direct children without following links. */
CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata);

/* Nonblocking advisory lock. Exclusive acquisition may create the lock file; shared
 * acquisition is read-only and requires an existing file. */
CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode);
void system_lock_release(SystemLock *lock);

#endif /* CUP_SYSTEM_H */
