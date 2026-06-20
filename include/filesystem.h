#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

#include <stddef.h>

#include "error.h"

/* Portable filesystem operations built on the platform system layer. */
CupError filesystem_ensure_directory(const char *path);
CupError filesystem_remove_tree(const char *path);
CupError filesystem_count_children(const char *path, const char *excluded_path, size_t *count);
CupError filesystem_clear_directory(const char *path, const char *preserved_path);
CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size);

#endif /* CUP_FILESYSTEM_H */
