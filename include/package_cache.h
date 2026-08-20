#ifndef CUP_PACKAGE_CACHE_H
#define CUP_PACKAGE_CACHE_H

/*
 * Resolves package cache paths and returns only checksum-authenticated opened artifacts. Stale
 * checksum metadata may be refreshed once before the request fails; extraction owns archive
 * structure and package-safety validation.
 */

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

typedef struct {
    PackageCacheSource source;
} PackageCacheResult;

/*
 * Return one opened, authenticated artifact whose stream is ready for extraction. A writable
 * result is cleared on every call; valid fetch attempts replace any artifact already owned by
 * the caller, while invalid input does not consume that artifact. `spec` must be an immutable
 * value published by package_artifact_spec_build() or package_artifact_spec_resolve_stable().
 */
CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCachePolicy policy,
                                      PackageCacheResult *result);

#endif /* CUP_PACKAGE_CACHE_H */
