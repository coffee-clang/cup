#ifndef CUP_FETCH_H
#define CUP_FETCH_H

#include <stddef.h>

#include "error.h"

CupError fetch_package(char *archive_path, size_t archive_path_size, const char *package_url, const char *component, const char *tool, const char *version);

#endif /* CUP_FETCH_H */