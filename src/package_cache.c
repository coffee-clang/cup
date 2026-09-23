/* Reuse or download one package archive; cache publication is opportunistic and never authority. */

#include "package_cache.h"

#include "download.h"
#include "layout.h"
#include "system.h"
#include "text.h"

#include <stdio.h>

static void warn_cache_unavailable(void) {
    fprintf(stderr, "Warning: package cache is unavailable; continuing with verified staging bytes.\n");
}

static CupError remove_disposable_path(const char *path) {
    SystemPathKind kind;
    CupError err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    return kind == SYSTEM_PATH_REGULAR_FILE ? system_remove_file(path) : CUP_OK;
}

static void discard_rejected_cache(VerifiedArtifact *artifact, const char *path) {
    CupError err = CUP_OK;

    if (artifact->file != NULL) {
        err = verified_artifact_discard(artifact);
    } else {
        SystemPathKind kind;
        if (system_get_path_kind(path, &kind) == CUP_OK && kind == SYSTEM_PATH_REGULAR_FILE) {
            err = system_remove_file(path);
        }
    }
    if (err != CUP_OK) {
        warn_cache_unavailable();
    }
}

static void publish_cache_best_effort(const VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec) {
    char cache_path[MAX_PATH_LEN];
    CupError err;

    err = layout_ensure_cache();
    if (err == CUP_OK) {
        err = layout_build_cache_path(cache_path, sizeof(cache_path), spec->artifact_sha256);
    }
    if (err == CUP_OK) {
        err = system_copy_file(artifact->path, cache_path);
    }
    if (err != CUP_OK) {
        warn_cache_unavailable();
    }
}

CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCacheSource *source) {
    ArtifactVerificationStatus status;
    char cache_path[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    CupError err;

    if (source != NULL) {
        *source = PACKAGE_CACHE_SOURCE_NONE;
    }
    if (artifact == NULL || spec == NULL || source == NULL ||
        !checksum_digest_is_canonical(spec->artifact_sha256)) {
        return CUP_ERR_INVALID_INPUT;
    }
    verified_artifact_release(artifact);

    err = layout_build_cache_path(cache_path, sizeof(cache_path), spec->artifact_sha256);
    if (err != CUP_OK) {
        return err;
    }
    err = verified_artifact_open(artifact, cache_path, spec, &status);
    if (err != CUP_OK) {
        warn_cache_unavailable();
        verified_artifact_release(artifact);
        status = ARTIFACT_VERIFY_MISSING;
    }
    if (status == ARTIFACT_VERIFY_VALID) {
        artifact->disposable = 0;
        *source = PACKAGE_CACHE_SOURCE_CACHE;
        return CUP_OK;
    }
    if (status != ARTIFACT_VERIFY_MISSING) {
        discard_rejected_cache(artifact, cache_path);
        verified_artifact_release(artifact);
    }

    err = layout_get_staging_dir(staging, sizeof(staging));
    if (err == CUP_OK) {
        err = system_make_unique_temp_path(staging, "artifact", temporary, sizeof(temporary));
    }
    if (err != CUP_OK) {
        return err;
    }
    err = download_file_checked(spec->package_url, temporary, DOWNLOAD_VALIDATE_ARCHIVE, NULL, NULL);
    if (err != CUP_OK) {
        (void)remove_disposable_path(temporary);
        return err;
    }
    err = verified_artifact_open(artifact, temporary, spec, &status);
    if (err != CUP_OK) {
        (void)remove_disposable_path(temporary);
        return err;
    }
    if (status != ARTIFACT_VERIFY_VALID) {
        fprintf(stderr, "Error: downloaded package failed SHA-256 verification.\n");
        if (artifact->file != NULL) {
            err = verified_artifact_discard(artifact);
        } else {
            err = remove_disposable_path(temporary);
        }
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }

    artifact->disposable = 1;
    publish_cache_best_effort(artifact, spec);
    *source = PACKAGE_CACHE_SOURCE_NETWORK;
    return CUP_OK;
}
