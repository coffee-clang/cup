#ifndef CUP_FETCH_H
#define CUP_FETCH_H

#include <stddef.h>

#include "error.h"
#include "package.h"

/* Resolve the package cache path and download the archive when needed. */
CupError fetch_package(char *archive_path, size_t archive_path_size,
    const char *package_url, const PackageIdentity *identity,
    const char *format, int force_download);

/* Download one non-package resource to a fixed destination. */
CupError fetch_resource(const char *url, const char *destination);

#endif /* CUP_FETCH_H */
