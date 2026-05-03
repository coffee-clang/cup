#ifndef CUP_FETCH_H
#define CUP_FETCH_H

#include <stddef.h>

#include "error.h"

CupError fetch_package(char *buffer, size_t size, const char *component, const char *tool, const char *platform, const char *resolved_release, const char *archive_format);

#endif /* CUP_FETCH_H */