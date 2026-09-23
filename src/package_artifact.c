/* Pin concrete catalog coordinates and keep verified cache bytes open through extraction. */

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
    PackageArchiveFormat format;
    char version[MAX_IDENTIFIER_LEN];
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
        err = package_archive_default_format(host_platform, &format);
    }
    return err == CUP_OK
               ? package_artifact_spec_build(
                     spec, catalog, &identity, package_archive_format_name(format))
               : err;
}

CupError package_artifact_spec_build(PackageArtifactSpec *spec,
                                     const PackageCatalog *catalog,
                                     const PackageIdentity *identity,
                                     const char *format_name) {
    PackageArtifactSpec candidate = {0};
    CupError err;

    if (spec == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(spec, 0, sizeof(*spec));
    if (catalog == NULL || identity == NULL ||
        package_identity_validate(identity, stderr) != CUP_OK || text_is_empty(format_name) ||
        package_archive_parse_format(format_name, &candidate.format) != CUP_OK ||
        !catalog->identity.valid || !checksum_digest_is_canonical(catalog->digest)) {
        return CUP_ERR_INVALID_INPUT;
    }

    candidate.identity = *identity;
    err = package_catalog_resolve_artifact(catalog,
                                           identity->component,
                                           identity->tool,
                                           identity->host_platform,
                                           identity->target_platform,
                                           identity->version,
                                           format_name,
                                           candidate.package_url,
                                           sizeof(candidate.package_url),
                                           candidate.artifact_sha256,
                                           sizeof(candidate.artifact_sha256));
    if (err != CUP_OK) {
        return err;
    }
    *spec = candidate;
    return CUP_OK;
}

CupError verified_artifact_open(VerifiedArtifact *artifact,
                                const char *path,
                                const PackageArtifactSpec *spec,
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
        !checksum_digest_is_canonical(spec->artifact_sha256) || status == NULL) {
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
    artifact->file = file;
    artifact->identity = identity;
    artifact->format = spec->format;
    if (text_copy(artifact->path, sizeof(artifact->path), path) != CUP_OK) {
        verified_artifact_release(artifact);
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (size == 0 || size > MAX_PACKAGE_DOWNLOAD_BYTES) {
        *status = ARTIFACT_VERIFY_REJECTED;
        return CUP_OK;
    }

    err = checksum_sha256_stream(file, artifact->digest, sizeof(artifact->digest));
    if (err != CUP_OK) {
        verified_artifact_release(artifact);
        return err;
    }
    *status = strcmp(artifact->digest, spec->artifact_sha256) == 0
                  ? ARTIFACT_VERIFY_VALID
                  : ARTIFACT_VERIFY_DIGEST_MISMATCH;
    return CUP_OK;
}

CupError verified_artifact_discard(VerifiedArtifact *artifact) {
    CupError err;

    if (artifact == NULL || artifact->file == NULL || !artifact->identity.valid ||
        text_is_empty(artifact->path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (fclose(artifact->file) != 0) {
        artifact->file = NULL;
        return CUP_ERR_FILESYSTEM;
    }
    artifact->file = NULL;
    err = system_remove_file_if_identity(artifact->path, &artifact->identity);
    verified_artifact_init(artifact);
    return err;
}
