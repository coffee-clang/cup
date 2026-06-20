#ifndef CUP_PATH_H
#define CUP_PATH_H

#include <stddef.h>

#include "error.h"

/* Join a parent path and one child path. */
CupError path_join(char *buffer, size_t size, const char *parent, const char *child);

/* Join a parent path and a validated relative path. */
CupError path_join_safe_relative(char *buffer, size_t size, const char *parent, const char *child);

/* Return the final segment of a path without modifying it. */
const char *path_last_segment(const char *path);

/* Validate a single filesystem segment or a restricted identifier. */
int path_is_safe_segment(const char *value);
int path_is_safe_identifier(const char *value);

/* Validate relative paths used inside packages and archives. */
int path_is_safe_relative(const char *path);

#endif /* CUP_PATH_H */
