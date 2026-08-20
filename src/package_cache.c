/*
 * Resolves package cache paths, authenticates cached/downloaded artifacts by checksum, and
 * refreshes stale checksum metadata at most once. Archive structure is owned by extraction.
 */

#include "package_cache.h"

#include "checksum.h"
#include "download.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

static CupError build_checksum_cache_path(const char *archive_path,
                                          char *checksum_path,
                                          size_t size) {
    char directory[MAX_PATH_LEN];
    CupError err;

    err = path_parent(directory, sizeof(directory), archive_path);
    return err == CUP_OK ? path_join(checksum_path, size, directory, "SHA256SUMS") : err;
}

static CupError package_cache_discard(const char *archive_path) {
    CupError err;
    SystemPathKind kind;
    int is_read_only;

    if (text_is_empty(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(archive_path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_read_only(archive_path, &is_read_only);
    if (err != CUP_OK) {
        return err;
    }
    if (is_read_only) {
        err = system_set_read_only(archive_path, 0);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = system_remove_file(archive_path);
    if (err != CUP_OK && is_read_only) {
        if (system_set_read_only(archive_path, 1) != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
    }

    return err;
}

/* Validate downloaded checksum metadata before it replaces the cached document. */
typedef struct {
    const char *archive_name;
} ChecksumDownloadValidation;

static CupError validate_checksum_download(const char *path, void *userdata) {
    const ChecksumDownloadValidation *validation = userdata;
    ChecksumDocument document;
    char expected[CHECKSUM_SHA256_HEX_LENGTH + 1];
    CupError err;

    if (validation == NULL || !path_is_safe_segment(validation->archive_name)) {
        return CUP_ERR_INVALID_INPUT;
    }
    checksum_document_init(&document);
    err = checksum_document_load(&document, path);
    if (err == CUP_OK) {
        err = checksum_document_find_expected(
            &document, validation->archive_name, expected, sizeof(expected));
    }
    checksum_document_free(&document);
    return err == CUP_OK ? CUP_OK : CUP_ERR_VALIDATION;
}

static CupError load_expected_digest(const char *checksum_path,
                                     const char *archive_name,
                                     char *expected,
                                     size_t expected_size) {
    ChecksumDocument document;
    CupError err;

    checksum_document_init(&document);
    err = checksum_document_load(&document, checksum_path);
    if (err == CUP_OK) {
        err = checksum_document_find_expected(&document, archive_name, expected, expected_size);
    }
    checksum_document_free(&document);
    return err;
}

static CupError refresh_checksum_document(const PackageArtifactSpec *spec,
                                          const char *archive_name,
                                          const char *checksum_path,
                                          int *checksum_refreshed) {
    ChecksumDownloadValidation validation;
    CupError err;

    validation.archive_name = archive_name;
    err = download_file_checked(spec->checksum_url,
                                checksum_path,
                                DOWNLOAD_VALIDATE_METADATA,
                                validate_checksum_download,
                                &validation);
    if (err == CUP_OK) {
        *checksum_refreshed = 1;
    }
    return err;
}

static CupError prepare_expected_digest(const PackageArtifactSpec *spec,
                                        const char *archive_name,
                                        const char *checksum_path,
                                        PackageCachePolicy policy,
                                        int *checksum_refreshed,
                                        char *expected,
                                        size_t expected_size) {
    SystemPathKind kind;
    CupError err;

    err = system_get_path_kind(checksum_path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (policy == PACKAGE_CACHE_REFRESH || kind == SYSTEM_PATH_MISSING) {
        err = refresh_checksum_document(spec, archive_name, checksum_path, checksum_refreshed);
        if (err != CUP_OK) {
            return err;
        }
    } else if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_VALIDATION;
    }

    err = load_expected_digest(checksum_path, archive_name, expected, expected_size);
    if (err != CUP_OK && policy == PACKAGE_CACHE_ALLOW && !*checksum_refreshed) {
        err = refresh_checksum_document(spec, archive_name, checksum_path, checksum_refreshed);
        if (err == CUP_OK) {
            err = load_expected_digest(checksum_path, archive_name, expected, expected_size);
        }
    }
    if (err != CUP_OK) {
        return err == CUP_ERR_INTERRUPT ? err : CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError discard_artifact_rejection(VerifiedArtifact *artifact,
                                           const char *archive_path,
                                           CupError original) {
    CupError discard_err;

    if (artifact->file != NULL) {
        discard_err = verified_artifact_discard(artifact);
    } else {
        discard_err = package_cache_discard(archive_path);
    }
    if (discard_err == CUP_OK) {
        return original;
    }
    if (discard_err == CUP_ERR_ROLLBACK) {
        return discard_err;
    }
    return original == CUP_ERR_COMMIT ? original : discard_err;
}

static CupError refresh_expected_and_reverify(VerifiedArtifact *artifact,
                                              const PackageArtifactSpec *spec,
                                              const char *archive_name,
                                              const char *checksum_path,
                                              int *checksum_refreshed,
                                              char *expected,
                                              size_t expected_size,
                                              ArtifactVerificationStatus *artifact_status) {
    CupError err;

    err = refresh_checksum_document(spec, archive_name, checksum_path, checksum_refreshed);
    if (err != CUP_OK) {
        return err;
    }

    err = load_expected_digest(checksum_path, archive_name, expected, expected_size);
    if (err != CUP_OK) {
        return err == CUP_ERR_INTERRUPT ? err : CUP_ERR_VALIDATION;
    }

    return verified_artifact_verify_expected(artifact, expected, artifact_status);
}

CupError package_cache_fetch_artifact(VerifiedArtifact *artifact,
                                      const PackageArtifactSpec *spec,
                                      PackageCachePolicy policy,
                                      PackageCacheResult *result) {
    ArtifactVerificationStatus artifact_status;
    CupError err;
    char archive_path[MAX_PATH_LEN];
    char checksum_path[MAX_PATH_LEN];
    char expected[CHECKSUM_SHA256_HEX_LENGTH + 1];
    const char *archive_name;
    const char *format_name;
    int checksum_refreshed = 0;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (artifact == NULL || spec == NULL || result == NULL ||
        (policy != PACKAGE_CACHE_ALLOW && policy != PACKAGE_CACHE_REFRESH)) {
        return CUP_ERR_INVALID_INPUT;
    }
    format_name = package_archive_format_name(spec->format);
    if (format_name == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    verified_artifact_release(artifact);

    err = layout_ensure_cache_parent(&spec->identity);
    if (err == CUP_OK) {
        err = layout_build_cache_archive_path(
            archive_path, sizeof(archive_path), &spec->identity, format_name);
    }
    if (err == CUP_OK) {
        err = build_checksum_cache_path(archive_path, checksum_path, sizeof(checksum_path));
    }
    if (err != CUP_OK) {
        return err;
    }
    archive_name = path_last_segment(archive_path);
    if (text_is_empty(archive_name)) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = prepare_expected_digest(
        spec, archive_name, checksum_path, policy, &checksum_refreshed, expected, sizeof(expected));
    if (err != CUP_OK) {
        return err;
    }

    if (policy == PACKAGE_CACHE_ALLOW) {
        err = verified_artifact_open(artifact, archive_path, spec, expected, &artifact_status);
        if (err != CUP_OK) {
            return err;
        }
        if (artifact_status == ARTIFACT_VERIFY_VALID) {
            result->source = PACKAGE_CACHE_SOURCE_CACHE;
            return CUP_OK;
        }

        if (artifact_status == ARTIFACT_VERIFY_DIGEST_MISMATCH && !checksum_refreshed) {
            err = refresh_expected_and_reverify(artifact,
                                                spec,
                                                archive_name,
                                                checksum_path,
                                                &checksum_refreshed,
                                                expected,
                                                sizeof(expected),
                                                &artifact_status);
            if (err != CUP_OK) {
                return discard_artifact_rejection(artifact, archive_path, err);
            }
            if (artifact_status == ARTIFACT_VERIFY_VALID) {
                result->source = PACKAGE_CACHE_SOURCE_CACHE;
                return CUP_OK;
            }
        }

        if (artifact_status != ARTIFACT_VERIFY_MISSING) {
            err = discard_artifact_rejection(artifact, archive_path, CUP_OK);
            if (err != CUP_OK) {
                return err;
            }
            if (artifact_status == ARTIFACT_VERIFY_WRONG_TYPE) {
                return CUP_ERR_FILESYSTEM;
            }
        }
    }

    err = download_file_checked(
        spec->package_url, archive_path, DOWNLOAD_VALIDATE_ARCHIVE, NULL, NULL);
    if (err != CUP_OK) {
        return err;
    }

    err = verified_artifact_open(artifact, archive_path, spec, expected, &artifact_status);
    if (err != CUP_OK) {
        return err;
    }
    if (artifact_status == ARTIFACT_VERIFY_DIGEST_MISMATCH && !checksum_refreshed) {
        err = refresh_expected_and_reverify(artifact,
                                            spec,
                                            archive_name,
                                            checksum_path,
                                            &checksum_refreshed,
                                            expected,
                                            sizeof(expected),
                                            &artifact_status);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: downloaded package failed SHA-256 verification.\n");
            return discard_artifact_rejection(artifact, archive_path, err);
        }
    }
    if (artifact_status != ARTIFACT_VERIFY_VALID) {
        fprintf(stderr, "Error: downloaded package failed SHA-256 verification.\n");
        return discard_artifact_rejection(artifact, archive_path, CUP_ERR_VALIDATION);
    }

    result->source = PACKAGE_CACHE_SOURCE_NETWORK;
    return CUP_OK;
}
