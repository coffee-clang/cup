#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

/*
 * Portable filesystem operations composed from system.h primitives. These helpers may
 * traverse multiple paths, but package, state and transaction policy stay in higher layers.
 */

#include <stddef.h>
#include <stdio.h>

#include "error.h"
#include "system.h"

typedef struct {
    unsigned char *data;
    size_t size;
    SystemPathIdentity identity;
} PersistentFileSnapshot;

typedef CupError (*FilesystemFileWriter)(FILE *file, const void *value);

void filesystem_snapshot_init(PersistentFileSnapshot *snapshot);
void filesystem_snapshot_release(PersistentFileSnapshot *snapshot);
CupError filesystem_snapshot_read(const char *path,
                                  size_t maximum_bytes,
                                  PersistentFileSnapshot *snapshot,
                                  int *missing);

/*
 * Serialize one managed file through a sibling temporary file and sync it before publication.
 * The create form never replaces an existing destination; the replace form does so atomically.
 * CUP_ERR_COMMIT means the new file may be visible, but directory durability is uncertain.
 */
CupError filesystem_publish_new_file(const char *directory,
                                     const char *temporary_prefix,
                                     const char *destination,
                                     int executable,
                                     FilesystemFileWriter writer,
                                     const void *value);
CupError filesystem_replace_file_atomically(const char *directory,
                                            const char *temporary_prefix,
                                            const char *destination,
                                            int executable,
                                            FilesystemFileWriter writer,
                                            const void *value);
/* Replace only the exact regular-file destination previously observed by the caller. */
CupError filesystem_replace_file_if_identity(const char *directory,
                                             const char *temporary_prefix,
                                             const char *destination,
                                             const SystemPathIdentity *expected_identity,
                                             int executable,
                                             FilesystemFileWriter writer,
                                             const void *value);

/* Create one missing directory, accepting an existing directory; its parent chain must exist. */
CupError filesystem_ensure_directory(const char *path);

/* Remove one path tree without following symbolic links. */
CupError filesystem_remove_tree(const char *path);

/* Enforce required executable and read-only permissions on one regular file. */
CupError filesystem_apply_required_permissions(const char *path, int executable, int read_only);

/* Count direct children, optionally excluding one exact child path. */
CupError filesystem_count_children(const char *path, const char *excluded_path, size_t *count);

/* Remove every direct child except an optional preserved path. */
CupError filesystem_clear_directory(const char *path, const char *preserved_path);

/* Move invalid data to a unique sibling backup. A missing path succeeds with an empty result. */
CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size);
/* Preserve only the exact regular-file or directory source previously observed by the caller. */
CupError filesystem_backup_invalid_if_identity(const char *path,
                                               const SystemPathIdentity *expected_identity,
                                               char *backup_path,
                                               size_t backup_size);

#endif /* CUP_FILESYSTEM_H */
