#ifndef CUP_PATH_H
#define CUP_PATH_H

/*
 * Module contract: Bounded path composition and validation of identifiers,
 * individual path segments, and package-relative paths.
 */

#include <stddef.h>

#include "error.h"

/* Join a parent and child path with the canonical '/' accepted on all hosts. */
CupError path_join(char *buffer, size_t size, const char *parent, const char *child);

/* Join only after validating child as a safe relative path. */
CupError path_join_safe_relative(char *buffer, size_t size, const char *parent, const char *child);

/* Return a pointer to the final path segment without modifying the input. */
const char *path_last_segment(const char *path);

/* Validate one segment or a stricter identifier accepted by text formats. */
int path_is_safe_segment(const char *value);
int path_is_safe_identifier(const char *value);

/* Validate a nonabsolute relative path with no traversal or empty segment. */
int path_is_safe_relative(const char *path);

#endif /* CUP_PATH_H */
