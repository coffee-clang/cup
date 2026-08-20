/*
 * Pins catalog-derived package coordinates and owns one cache file from digest verification to
 * libarchive consumption. Pathname replacement after opening cannot change the bytes consumed.
 */

#include "package_artifact.h"

#include "checksum.h"
#include "text.h"

#include <string.h>

void verified_artifact_init(VerifiedArtifact *artifact) {
    if (artifact != NULL) {
        memset(artifact, 0, sizeof(*artifact));
    }
}

void verified_artifact_release(VerifiedArtifact *artifact) {
    if (artifact == NULL) {
        return;
    }
    if (artifact->file != NULL) {
        fclose(artifact->file);
    }
    verified_artifact_init(artifact);
}

CupError package_artifact_spec_resolve_stable(PackageArtifactSpec *spec,
                                              const PackageCatalog *catalog,
                                              const char *component,
                                              const char *tool,
                                              const char *host_platform,
                                              const char *target_platform) {
    PackageIdentity identity;
    char version[MAX_IDENTIFIER_LEN];
    char format[MAX_IDENTIFIER_LEN];
    CupError err;

    if (spec == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(spec, 0, sizeof(*spec));
    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) ||
        text_is_empty(host_platform) || text_is_empty(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = package_catalog_resolve_stable(catalog,
                                         version,
                                         sizeof(version),
                                         component,
                                         tool,
                                         host_platform,
                                         target_platform);
    if (err == CUP_OK) {
        err = package_identity_init(
            &identity, component, tool, host_platform, target_platform, version);
    }
    if (err == CUP_OK) {
        err = package_catalog_get_default_format(catalog,
                                                 format,
                                                 sizeof(format),
                                                 component,
                                                 tool,
                                                 host_platform,
                                                 target_platform);
    }
    if (err == CUP_OK) {
        err = package_artifact_spec_build(spec, catalog, &identity, format);
    }
    return err;
}

CupError package_artifact_spec_build(PackageArtifactSpec *spec,
                                     const PackageCatalog *catalog,
                                     const PackageIdentity *identity,
                                     const char *format_name) {
    PackageArtifactSpec candidate = {0};
    PackageArchiveFormat format;
    CupError err;
    int available;

    if (spec == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (catalog == NULL || identity == NULL ||
        package_identity_validate(identity, stderr) != CUP_OK || text_is_empty(format_name) ||
        package_archive_parse_format(format_name, &format) != CUP_OK ||
        !catalog->identity.valid || !checksum_digest_is_canonical(catalog->digest)) {
        memset(spec, 0, sizeof(*spec));
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_catalog_has_version(catalog,
                                      identity->component,
                                      identity->tool,
                                      identity->host_platform,
                                      identity->target_platform,
                                      identity->version,
                                      &available);
    if (err != CUP_OK || !available) {
        memset(spec, 0, sizeof(*spec));
        return err != CUP_OK ? err : CUP_ERR_NOT_AVAILABLE;
    }
    err = package_catalog_has_format(catalog,
                                     identity->component,
                                     identity->tool,
                                     identity->host_platform,
                                     identity->target_platform,
                                     format_name,
                                     &available);
    if (err != CUP_OK || !available) {
        memset(spec, 0, sizeof(*spec));
        return err != CUP_OK ? err : CUP_ERR_NOT_AVAILABLE;
    }

    candidate.identity = *identity;
    candidate.format = format;
    err = package_catalog_build_url(catalog,
                                    candidate.package_url,
                                    sizeof(candidate.package_url),
                                    identity->component,
                                    identity->tool,
                                    identity->host_platform,
                                    identity->target_platform,
                                    identity->version,
                                    format_name);
    if (err == CUP_OK) {
        err = package_catalog_build_checksum_url(catalog,
                                                 candidate.checksum_url,
                                                 sizeof(candidate.checksum_url),
                                                 identity->component,
                                                 identity->tool,
                                                 identity->host_platform,
                                                 identity->target_platform,
                                                 identity->version);
    }
    if (err != CUP_OK) {
        memset(spec, 0, sizeof(*spec));
        return err;
    }
    *spec = candidate;
    return CUP_OK;
}

CupError verified_artifact_verify_expected(VerifiedArtifact *artifact,
                                           const char *expected_digest,
                                           ArtifactVerificationStatus *status) {
    if (status != NULL) {
        *status = ARTIFACT_VERIFY_NONE;
    }
    if (artifact == NULL || artifact->file == NULL || !artifact->identity.valid ||
        !checksum_digest_is_canonical(artifact->digest) ||
        !checksum_digest_is_canonical(expected_digest) || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strcmp(artifact->digest, expected_digest) != 0) {
        *status = ARTIFACT_VERIFY_DIGEST_MISMATCH;
        return CUP_OK;
    }

    *status = ARTIFACT_VERIFY_VALID;
    return CUP_OK;
}

CupError verified_artifact_open(VerifiedArtifact *artifact,
                                const char *path,
                                const PackageArtifactSpec *spec,
                                const char *expected_digest,
                                ArtifactVerificationStatus *status) {
    FILE *file = NULL;
    SystemPathIdentity identity;
    uint64_t size;
    CupError err;
    int missing;

    if (status != NULL) {
        *status = ARTIFACT_VERIFY_NONE;
    }
    if (artifact == NULL || text_is_empty(path) || spec == NULL ||
        !checksum_digest_is_canonical(expected_digest) || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    verified_artifact_release(artifact);
    memset(&identity, 0, sizeof(identity));

    {
        SystemPathKind kind;
        err = system_get_path_kind(path, &kind);
        if (err != CUP_OK) {
            return err;
        }
        if (kind == SYSTEM_PATH_MISSING) {
            *status = ARTIFACT_VERIFY_MISSING;
            return CUP_OK;
        }
        if (kind != SYSTEM_PATH_REGULAR_FILE) {
            *status = ARTIFACT_VERIFY_WRONG_TYPE;
            return CUP_OK;
        }
    }

    err = system_open_regular_file(path, &file, &identity, &size, &missing);
    if (err != CUP_OK) {
        return err;
    }
    if (missing) {
        *status = ARTIFACT_VERIFY_MISSING;
        return CUP_OK;
    }
    if (identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        fclose(file);
        *status = ARTIFACT_VERIFY_WRONG_TYPE;
        return CUP_OK;
    }
    if (size == 0 || size > MAX_PACKAGE_DOWNLOAD_BYTES) {
        fclose(file);
        *status = ARTIFACT_VERIFY_REJECTED;
        return CUP_OK;
    }

    artifact->file = file;
    artifact->identity = identity;
    artifact->format = spec->format;
    if (text_copy(artifact->path, sizeof(artifact->path), path) != CUP_OK) {
        verified_artifact_release(artifact);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    err = checksum_sha256_stream(file, artifact->digest, sizeof(artifact->digest));
    if (err != CUP_OK) {
        verified_artifact_release(artifact);
        return err;
    }
    err = verified_artifact_verify_expected(artifact, expected_digest, status);
    if (err != CUP_OK) {
        verified_artifact_release(artifact);
    }
    return err;
}

CupError verified_artifact_discard(VerifiedArtifact *artifact) {
    char path[MAX_PATH_LEN];
    SystemPathIdentity identity;
    CupError err;

    if (artifact == NULL || artifact->file == NULL || text_is_empty(artifact->path) ||
        !artifact->identity.valid) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (text_copy(path, sizeof(path), artifact->path) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    identity = artifact->identity;
    verified_artifact_release(artifact);
    err = system_remove_file_if_identity(path, &identity);
    return err;
}
