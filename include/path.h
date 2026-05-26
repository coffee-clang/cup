#ifndef CUP_PATH_H
#define CUP_PATH_H

#include <stddef.h>

#include "error.h"

CupError path_join(char *buffer, size_t size, const char *parent, const char *child);
CupError path_join_safe_relative(char *buffer, size_t size, const char *parent, const char *child);
int path_has_parent_ref(const char *path);
int path_is_safe_relative(const char *path);

#endif /* CUP_PATH_H */