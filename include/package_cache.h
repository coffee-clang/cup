#ifndef CUP_PACKAGE_CACHE_H
#define CUP_PACKAGE_CACHE_H

/*
 * Resolves package cache paths and accepts an archive only after checksum and structural
 * validation. Stale checksum metadata may be refreshed once before the request fails.
 */

#include <stddef.h>

#include "error.h"
#include "package.h"

typedef enum {
    PACKAGE_CACHE_ALLOW,
    PACKAGE_CACHE_REFRESH
} PackageCachePolicy;

typedef enum {
    PACKAGE_CACHE_SOURCE_NONE,
    PACKAGE_CACHE_SOURCE_CACHE,
    PACKAGE_CACHE_SOURCE_NETWORK
} PackageCacheSource;

/* Resolve and validate one archive, downloading only when the selected policy permits it. */
CupError package_cache_fetch(char *archive_path,
                             size_t archive_path_size,
                             const char *package_url,
                             const char *checksum_url,
                             const PackageIdentity *identity,
                             const char *format,
                             PackageCachePolicy policy,
                             PackageCacheSource *source);

/* Remove one cache archive after validation or installation has made it unusable. */
CupError package_cache_discard(const char *archive_path);

#endif /* CUP_PACKAGE_CACHE_H */
