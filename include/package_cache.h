#ifndef CUP_PACKAGE_CACHE_H
#define CUP_PACKAGE_CACHE_H

/* Persistent archive reuse authenticated directly by the catalog artifact digest. */

#include "error.h"
#include "package_artifact.h"

typedef enum {
    PACKAGE_CACHE_SOURCE_NONE,
    PACKAGE_CACHE_SOURCE_CACHE,
    PACKAGE_CACHE_SOURCE_NETWORK
} PackageCacheSource;

CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCacheSource *source);

#endif /* CUP_PACKAGE_CACHE_H */
