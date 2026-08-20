#ifndef CUP_PACKAGE_ARTIFACT_H
#define CUP_PACKAGE_ARTIFACT_H

/*
 * Immutable package coordinates derived from one catalog snapshot and one opened cache artifact
 * whose bytes remain owned from SHA-256 verification through archive extraction.
 */

#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "package.h"
#include "package_archive.h"
#include "package_catalog.h"
#include "system.h"
#include "checksum.h"

typedef struct {
    PackageIdentity identity;
    PackageArchiveFormat format;
    char package_url[MAX_CATALOG_URL_LEN];
    char checksum_url[MAX_CATALOG_URL_LEN];
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
} VerifiedArtifact;

void verified_artifact_init(VerifiedArtifact *artifact);
void verified_artifact_release(VerifiedArtifact *artifact);

/* Successful resolution/build publishes one complete spec; failure clears a writable output. */
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

/* `spec` must be an immutable value published by one of the builders above. */
CupError verified_artifact_open(VerifiedArtifact *artifact,
                                const char *path,
                                const PackageArtifactSpec *spec,
                                const char *expected_digest,
                                ArtifactVerificationStatus *status);

/*
 * Rebind refreshed checksum metadata to the already opened bytes without reopening the path.
 * Operational verification failure preserves the opened artifact so its exact identity remains
 * available to the caller for discard; verified_artifact_open() still owns cleanup on open.
 */
CupError verified_artifact_verify_expected(VerifiedArtifact *artifact,
                                           const char *expected_digest,
                                           ArtifactVerificationStatus *status);

/* Remove the pathname only when it still names the opened artifact. */
CupError verified_artifact_discard(VerifiedArtifact *artifact);

#endif /* CUP_PACKAGE_ARTIFACT_H */
