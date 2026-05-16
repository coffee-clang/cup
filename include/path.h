#ifndef CUP_PATH_H
#define CUP_PATH_H

#include <stddef.h>

#include "error.h"

CupError path_join(char *buffer, size_t size, const char *parent, const char *child);
int path_has_parent_ref(const char *path);

#endif /* CUP_PATH_H */