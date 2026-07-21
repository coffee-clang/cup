/*
 * Resolves package cache paths, validates archives and checksums, and refreshes stale cache
 * metadata at most once.
 */

#include "package_cache.h"

#include "checksum.h"
#include "download.h"
#include "layout.h"
#include "package_archive.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

static CupError build_checksum_cache_path(const char *archive_path,
                                          char *checksum_path,
                                          size_t size) {
    const char *slash = strrchr(archive_path, '/');
    size_t directory_length;
    char directory[MAX_PATH_LEN];

    if (slash == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    directory_length = (size_t)(slash - archive_path);
    if (directory_length == 0 || directory_length >= sizeof(directory)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(directory, archive_path, directory_length);
    directory[directory_length] = '\0';
    return path_join(checksum_path, size, directory, "SHA256SUMS");
}

/* Package cache and checksum validation. */
static const char *archive_filename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static CupError checksum_has_archive_entry(const char *checksum_path, const char *archive_path) {
    char expected[SHA256_HEX_LENGTH + 1];

    return checksum_find_expected(
        checksum_path, archive_filename(archive_path), expected, sizeof(expected));
}

static CupError verify_cached_archive(const char *checksum_path,
                                      const char *archive_path,
                                      const char *format,
                                      int *valid) {
    CupError err;
    int archive_valid;
    int checksum_matches;

    *valid = 0;
    err = package_archive_is_valid(archive_path, format, &archive_valid);
    if (err != CUP_OK || !archive_valid) {
        return err;
    }
    err = checksum_verify_file(
        checksum_path, archive_filename(archive_path), archive_path, &checksum_matches);
    if (err != CUP_OK) {
        return err;
    }
    *valid = checksum_matches;
    return CUP_OK;
}

static CupError refresh_checksum_metadata(const char *checksum_url,
                                          const char *checksum_path,
                                          int *checksum_refreshed) {
    CupError err = download_file(checksum_url, checksum_path, DOWNLOAD_VALIDATE_METADATA);

    if (err == CUP_OK) {
        *checksum_refreshed = 1;
    }
    return err;
}

static CupError prepare_checksum_metadata(const char *checksum_url,
                                          const char *checksum_path,
                                          const char *archive_path,
                                          PackageCachePolicy cache_policy,
                                          int *checksum_refreshed) {
    SystemPathKind checksum_kind;
    CupError err;

    err = system_get_path_kind(checksum_path, &checksum_kind);
    if (err != CUP_OK) {
        return err;
    }
    if (cache_policy == PACKAGE_CACHE_REFRESH || checksum_kind == SYSTEM_PATH_MISSING) {
        err = refresh_checksum_metadata(checksum_url, checksum_path, checksum_refreshed);
        if (err != CUP_OK) {
            return err;
        }
    } else if (checksum_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_FILESYSTEM;
    }

    err = checksum_has_archive_entry(checksum_path, archive_path);
    if (err != CUP_OK && cache_policy == PACKAGE_CACHE_ALLOW) {
        CupError refresh_err =
            refresh_checksum_metadata(checksum_url, checksum_path, checksum_refreshed);

        if (refresh_err != CUP_OK) {
            return refresh_err;
        }
        err = checksum_has_archive_entry(checksum_path, archive_path);
    }
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: package checksum metadata has no unique entry "
                "for '%s'.\n",
                archive_filename(archive_path));
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static int cache_validation_error_is_expected(CupError err) {
    return err == CUP_OK || err == CUP_ERR_FILESYSTEM || err == CUP_ERR_ARCHIVE ||
           err == CUP_ERR_VALIDATION;
}

static CupError try_cached_archive(const char *checksum_url,
                                   const char *checksum_path,
                                   const char *archive_path,
                                   const char *format,
                                   int *checksum_refreshed,
                                   int *reused) {
    CupError err;
    int valid_archive;

    *reused = 0;
    err = verify_cached_archive(checksum_path, archive_path, format, &valid_archive);
    if (err == CUP_OK && valid_archive) {
        *reused = 1;
        return CUP_OK;
    }

    /* A digest mismatch may mean the shared checksum metadata is stale. Refresh it once. */
    if (err == CUP_OK && !valid_archive && !*checksum_refreshed) {
        err = refresh_checksum_metadata(checksum_url, checksum_path, checksum_refreshed);
        if (err != CUP_OK) {
            return err;
        }
        if (checksum_has_archive_entry(checksum_path, archive_path) != CUP_OK) {
            return CUP_ERR_VALIDATION;
        }
        err = verify_cached_archive(checksum_path, archive_path, format, &valid_archive);
        if (err == CUP_OK && valid_archive) {
            *reused = 1;
            return CUP_OK;
        }
    }

    if (!cache_validation_error_is_expected(err)) {
        return err;
    }
    return package_cache_discard(archive_path);
}

static CupError fetch_and_verify_archive(const char *package_url,
                                         const char *checksum_url,
                                         const char *checksum_path,
                                         const char *archive_path,
                                         const char *format,
                                         int *checksum_refreshed) {
    CupError err;
    int valid_archive;

    err = download_file(package_url, archive_path, DOWNLOAD_VALIDATE_ARCHIVE);
    if (err != CUP_OK) {
        return err;
    }

    err = verify_cached_archive(checksum_path, archive_path, format, &valid_archive);
    if ((err != CUP_OK || !valid_archive) && !*checksum_refreshed) {
        CupError refresh_err =
            refresh_checksum_metadata(checksum_url, checksum_path, checksum_refreshed);

        if (refresh_err == CUP_OK &&
            checksum_has_archive_entry(checksum_path, archive_path) == CUP_OK) {
            err = verify_cached_archive(checksum_path, archive_path, format, &valid_archive);
        } else {
            err = refresh_err == CUP_OK ? CUP_ERR_VALIDATION : refresh_err;
        }
    }
    if (err == CUP_OK && valid_archive) {
        return CUP_OK;
    }

    {
        CupError discard_err = package_cache_discard(archive_path);

        fprintf(stderr, "Error: downloaded package failed SHA-256 verification.\n");
        if (discard_err != CUP_OK) {
            return discard_err;
        }
    }
    return err != CUP_OK ? err : CUP_ERR_VALIDATION;
}

CupError package_cache_fetch(char *archive_path,
                             size_t archive_path_size,
                             const char *package_url,
                             const char *checksum_url,
                             const PackageIdentity *identity,
                             const char *format,
                             PackageCachePolicy cache_policy,
                             PackageCacheSource *source) {
    CupError err;
    char checksum_path[MAX_PATH_LEN];
    int checksum_refreshed = 0;
    int reused;

    if (source == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *source = PACKAGE_CACHE_SOURCE_NONE;
    if (archive_path == NULL || archive_path_size == 0 || text_is_empty(package_url) ||
        text_is_empty(checksum_url) || identity == NULL || text_is_empty(format) ||
        (cache_policy != PACKAGE_CACHE_ALLOW && cache_policy != PACKAGE_CACHE_REFRESH)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Resolve one identity-bound archive path and its shared checksum metadata. */
    err = layout_ensure_cache_parent(identity);
    if (err == CUP_OK) {
        err = layout_build_cache_archive_path(archive_path, archive_path_size, identity, format);
    }
    if (err == CUP_OK) {
        err = build_checksum_cache_path(archive_path, checksum_path, sizeof(checksum_path));
    }
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_checksum_metadata(checksum_url,
                                    checksum_path,
                                    archive_path,
                                    cache_policy,
                                    &checksum_refreshed);
    if (err != CUP_OK) {
        return err;
    }

    if (cache_policy == PACKAGE_CACHE_ALLOW) {
        err = try_cached_archive(checksum_url,
                                 checksum_path,
                                 archive_path,
                                 format,
                                 &checksum_refreshed,
                                 &reused);
        if (err != CUP_OK || reused) {
            if (reused) {
                *source = PACKAGE_CACHE_SOURCE_CACHE;
            }
            return err;
        }
    }

    err = fetch_and_verify_archive(package_url,
                                   checksum_url,
                                   checksum_path,
                                   archive_path,
                                   format,
                                   &checksum_refreshed);
    if (err == CUP_OK) {
        *source = PACKAGE_CACHE_SOURCE_NETWORK;
    }
    return err;
}

CupError package_cache_discard(const char *archive_path) {
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
