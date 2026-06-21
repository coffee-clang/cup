#ifndef CUP_FETCH_H
#define CUP_FETCH_H

#include <stddef.h>

#include "error.h"
#include "package.h"

typedef enum {
    FETCH_VALIDATE_NONEMPTY,
    FETCH_VALIDATE_ARCHIVE
} FetchValidation;

typedef enum {
    FETCH_ALLOW_CACHE,
    FETCH_REFRESH_CACHE
} FetchCachePolicy;

typedef enum {
    FETCH_SOURCE_CACHE,
    FETCH_SOURCE_NETWORK
} FetchSource;

/* Download one HTTPS resource and atomically replace the destination. */
CupError fetch_file(const char *url, const char *destination,
    FetchValidation validation);

/* Resolve one package cache path and return a fully validated archive. */
CupError fetch_package(char *archive_path, size_t archive_path_size,
    const char *package_url, const PackageIdentity *identity,
    const char *format, FetchCachePolicy cache_policy, FetchSource *source);

/* Remove an archive that failed extraction or package validation. */
CupError fetch_discard_cached_package(const char *archive_path);

#endif /* CUP_FETCH_H */
