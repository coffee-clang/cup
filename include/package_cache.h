#ifndef CUP_PACKAGE_CACHE_H
#define CUP_PACKAGE_CACHE_H

/* Resolve cache paths and return only checksum-authenticated open artifacts. Checksum metadata
 * may be refreshed once; archive/package validation belongs to extraction. */

#include "error.h"
#include "package_artifact.h"

typedef enum {
    PACKAGE_CACHE_ALLOW,
    PACKAGE_CACHE_REFRESH
} PackageCachePolicy;

typedef enum {
    PACKAGE_CACHE_SOURCE_NONE,
    PACKAGE_CACHE_SOURCE_CACHE,
    PACKAGE_CACHE_SOURCE_NETWORK
} PackageCacheSource;

/* Return an authenticated open artifact ready for extraction. A valid fetch replaces any
 * previous artifact; invalid input leaves it untouched. `spec` must come from a spec builder. */
CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCachePolicy policy,
                                      PackageCacheSource *source);

#endif /* CUP_PACKAGE_CACHE_H */
