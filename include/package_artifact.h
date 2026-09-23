#ifndef CUP_PACKAGE_ARTIFACT_H
#define CUP_PACKAGE_ARTIFACT_H

/* Immutable artifact coordinates pinned to one validated catalog snapshot. */

#include <stdio.h>

#include "checksum.h"
#include "constants.h"
#include "error.h"
#include "package.h"
#include "package_archive.h"
#include "package_catalog.h"
#include "system.h"

typedef struct {
    PackageIdentity identity;
    PackageArchiveFormat format;
    char package_url[MAX_CATALOG_URL_LEN];
    char artifact_sha256[CHECKSUM_SHA256_HEX_LENGTH + 1];
} PackageArtifactSpec;

typedef enum {
    ARTIFACT_VERIFY_NONE,
    ARTIFACT_VERIFY_VALID,
    ARTIFACT_VERIFY_MISSING,
    ARTIFACT_VERIFY_WRONG_TYPE,
    ARTIFACT_VERIFY_REJECTED,
    ARTIFACT_VERIFY_DIGEST_MISMATCH
} ArtifactVerificationStatus;

typedef struct {
    FILE *file;
    SystemPathIdentity identity;
    PackageArchiveFormat format;
    char path[MAX_PATH_LEN];
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    int disposable;
} VerifiedArtifact;

void verified_artifact_init(VerifiedArtifact *artifact);
void verified_artifact_release(VerifiedArtifact *artifact);

CupError package_artifact_spec_resolve_stable(PackageArtifactSpec *spec,
                                              const PackageCatalog *catalog,
                                              const char *component,
                                              const char *tool,
                                              const char *host_platform,
                                              const char *target_platform);
CupError package_artifact_spec_build(PackageArtifactSpec *spec,
                                     const PackageCatalog *catalog,
                                     const PackageIdentity *identity,
                                     const char *format_name);

CupError verified_artifact_open(VerifiedArtifact *artifact,
                                const char *path,
                                const PackageArtifactSpec *spec,
                                ArtifactVerificationStatus *status);
CupError verified_artifact_discard(VerifiedArtifact *artifact);

#endif /* CUP_PACKAGE_ARTIFACT_H */
