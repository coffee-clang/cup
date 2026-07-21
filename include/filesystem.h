#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

/*
 * Portable composite filesystem operations built on system.h. These helpers may traverse
 * multiple paths but do not own package, state, or transaction policy.
 */

#include <stddef.h>

#include "error.h"

/* Create path and any missing ancestors, accepting an existing directory. */
CupError filesystem_ensure_directory(const char *path);

/* Remove one path tree without following symbolic links. */
CupError filesystem_remove_tree(const char *path);

/* Count direct children, optionally excluding one exact child path. */
CupError filesystem_count_children(const char *path, const char *excluded_path, size_t *count);

/* Remove every direct child except an optional preserved path. */
CupError filesystem_clear_directory(const char *path, const char *preserved_path);

/* Move invalid data to a unique sibling backup and return its final path. */
CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size);

#endif /* CUP_FILESYSTEM_H */
