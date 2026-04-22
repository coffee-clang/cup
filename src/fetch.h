#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

#include "error.h"

CupError download_package(const char *url, const char *dst_path);
CupError fetch_package(char *buffer, size_t size, const char *component, const char *tool, const char *resolved_release, const char *archive_format);

#endif