#include "fetch.h"

#include "ca_bundle.h"
#include "checksum.h"
#include "constants.h"
#include "path.h"
#include "interrupt.h"
#include "layout.h"
#include "package_archive.h"
#include "system.h"
#include "text.h"

#include <curl/curl.h>
#if defined(CUP_USE_OPENSSL_INIT)
#include <openssl/ssl.h>
#endif
#include <stdio.h>
#include <string.h>

#if defined(CUP_USE_EMBEDDED_CA_BUNDLE) && LIBCURL_VERSION_NUM < 0x074D00
#error "CUP_USE_EMBEDDED_CA_BUNDLE requires libcurl >= 7.77.0"
#endif

static void initialize_tls_runtime(void) {
#if defined(CUP_USE_OPENSSL_INIT)
    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
#endif
}

static CupError configure_tls_trust(CURL *curl) {
#if defined(CUP_USE_EMBEDDED_CA_BUNDLE)
    struct curl_blob ca_blob;
    CURLcode result;

    ca_blob.data = (void *)cup_ca_bundle;
    ca_blob.len = cup_ca_bundle_len;
    ca_blob.flags = CURL_BLOB_NOCOPY;

    result = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
    if (result != CURLE_OK) {
        fprintf(stderr, "Error: could not configure embedded CA bundle: %s.\n",
            curl_easy_strerror(result));
        return CUP_ERR_FETCH;
    }
#else
    (void)curl;
#endif

    return CUP_OK;
}

typedef struct {
    FILE *file;
    curl_off_t limit;
    curl_off_t written;
    int too_large;
    int write_failed;
} DownloadWriter;

static size_t write_file_callback(void *data, size_t size,
    size_t count, void *userdata) {
    DownloadWriter *writer = userdata;
    size_t bytes;

    if (writer == NULL || writer->file == NULL) {
        return 0;
    }
    if (size != 0 && count > (size_t)-1 / size) {
        writer->write_failed = 1;
        return 0;
    }

    bytes = size * count;
    if ((curl_off_t)bytes > writer->limit - writer->written) {
        writer->too_large = 1;
        return 0;
    }
    if (fwrite(data, 1, bytes, writer->file) != bytes) {
        writer->write_failed = 1;
        return 0;
    }
    writer->written += (curl_off_t)bytes;
    return bytes;
}

static curl_off_t validation_limit(FetchValidation validation) {
    switch (validation) {
        case FETCH_VALIDATE_METADATA:
        case FETCH_VALIDATE_NONEMPTY:
            return (curl_off_t)MAX_METADATA_DOWNLOAD_BYTES;
        case FETCH_VALIDATE_BINARY:
            return (curl_off_t)MAX_BINARY_DOWNLOAD_BYTES;
        case FETCH_VALIDATE_ARCHIVE:
            return (curl_off_t)MAX_PACKAGE_DOWNLOAD_BYTES;
    }
    return 0;
}

static long validation_timeout(FetchValidation validation) {
    return validation == FETCH_VALIDATE_ARCHIVE ? 7200L : 300L;
}

static int progress_callback(void *userdata, curl_off_t download_total,
    curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    (void)userdata;
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;

    return interrupt_requested() ? 1 : 0;
}

#define SETOPT(handle, option, value) do { \
    CURLcode setopt_result = curl_easy_setopt((handle), (option), (value)); \
    if (setopt_result != CURLE_OK) { \
        fprintf(stderr, "Error: could not configure libcurl option %s: %s.\n", \
            #option, curl_easy_strerror(setopt_result)); \
        result = setopt_result; \
        goto cleanup; \
    } \
} while (0)

static CupError get_parent_path(const char *path, char *parent, size_t size) {
    char *slash;

    if (text_copy(parent, size, path) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return text_format(parent, size, ".");
    }

    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    return CUP_OK;
}

static CupError remove_temporary_download(const char *path,
    CupError original_error) {
    return system_remove_file(path) == CUP_OK
        ? original_error : CUP_ERR_TEMPORARY;
}

static CupError validate_download(const char *path,
    FetchValidation validation) {
    CupError err;
    long long size;
    int is_regular_file;
    int is_valid_archive;

    err = system_is_regular_file(path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular_file) {
        return validation == FETCH_VALIDATE_ARCHIVE
            ? CUP_ERR_ARCHIVE : CUP_ERR_FETCH;
    }

    err = system_file_size(path, &size);
    if (err != CUP_OK) {
        return err;
    }
    if (size <= 0) {
        fprintf(stderr, "Error: downloaded resource is empty.\n");
        return validation == FETCH_VALIDATE_ARCHIVE
            ? CUP_ERR_ARCHIVE : CUP_ERR_FETCH;
    }

    if (validation != FETCH_VALIDATE_ARCHIVE) {
        return CUP_OK;
    }

    err = package_archive_is_valid(path, &is_valid_archive);
    if (err != CUP_OK) {
        return err;
    }

    return is_valid_archive ? CUP_OK : CUP_ERR_ARCHIVE;
}

static CupError prepare_destination(const char *path, int *restore_read_only) {
    CupError err;
    SystemPathKind kind;
    int is_read_only;

    *restore_read_only = 0;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        fprintf(stderr, "Error: download destination '%s' is not a regular file.\n",
            path);
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_read_only(path, &is_read_only);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_read_only) {
        return CUP_OK;
    }

    err = system_set_read_only(path, 0);
    if (err == CUP_OK) {
        *restore_read_only = 1;
    }
    return err;
}

static CupError commit_download(const char *temporary_path,
    const char *destination) {
    CupError err;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int restore_read_only;

    err = prepare_destination(destination, &restore_read_only);
    if (err != CUP_OK) {
        return err;
    }

    err = system_replace_file(temporary_path, destination, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED && restore_read_only) {
        if (system_set_read_only(destination, 1) != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
    }

    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

CupError fetch_file(const char *url, const char *destination,
    FetchValidation validation) {
    CURL *curl = NULL;
    CURLcode result = CURLE_OK;
    CURLcode info_result = CURLE_OK;
    FILE *file = NULL;
    DownloadWriter writer = {0};
    CupError sync_err = CUP_OK;
    CupError err;
    char error_buffer[CURL_ERROR_SIZE];
    char parent[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN] = "";
    long response_code = 0;
    int close_status = 0;

    if (text_is_empty(url) || text_is_empty(destination) ||
        (validation != FETCH_VALIDATE_NONEMPTY &&
            validation != FETCH_VALIDATE_METADATA &&
            validation != FETCH_VALIDATE_BINARY &&
            validation != FETCH_VALIDATE_ARCHIVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_parent_path(destination, parent, sizeof(parent));
    if (err != CUP_OK) {
        return err;
    }

    initialize_tls_runtime();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return CUP_ERR_FETCH;
    }

    err = system_create_temp_file(parent, "download", temporary_path,
        sizeof(temporary_path), &file);
    if (err != CUP_OK) {
        curl_global_cleanup();
        return CUP_ERR_FETCH;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        int close_failed = fclose(file) != 0;
        CupError cleanup_error = system_remove_file(temporary_path);

        curl_global_cleanup();
        return close_failed || cleanup_error != CUP_OK
            ? CUP_ERR_TEMPORARY : CUP_ERR_FETCH;
    }

    writer.file = file;
    writer.limit = validation_limit(validation);
    error_buffer[0] = '\0';
    err = configure_tls_trust(curl);
    if (err != CUP_OK) {
        result = CURLE_FAILED_INIT;
        goto cleanup;
    }

    SETOPT(curl, CURLOPT_ERRORBUFFER, error_buffer);
    SETOPT(curl, CURLOPT_URL, url);
    SETOPT(curl, CURLOPT_FOLLOWLOCATION, 1L);
    SETOPT(curl, CURLOPT_MAXREDIRS, 10L);
    SETOPT(curl, CURLOPT_FAILONERROR, 1L);
    SETOPT(curl, CURLOPT_USERAGENT, "cup");
    SETOPT(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SETOPT(curl, CURLOPT_ACCEPT_ENCODING, "");
    SETOPT(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    SETOPT(curl, CURLOPT_TIMEOUT, validation_timeout(validation));
    SETOPT(curl, CURLOPT_MAXFILESIZE_LARGE, writer.limit);
    SETOPT(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    SETOPT(curl, CURLOPT_LOW_SPEED_TIME, 60L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SETOPT(curl, CURLOPT_PROTOCOLS_STR, "https");
    SETOPT(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    SETOPT(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    SETOPT(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    SETOPT(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    SETOPT(curl, CURLOPT_WRITEDATA, &writer);
    SETOPT(curl, CURLOPT_NOPROGRESS, 0L);
    SETOPT(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

    result = curl_easy_perform(curl);
    info_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
        &response_code);

cleanup:
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }

    if (file != NULL) {
        sync_err = system_sync_file(file);
        close_status = fclose(file);
        file = NULL;
    }
    curl_global_cleanup();

    if (result == CURLE_ABORTED_BY_CALLBACK && interrupt_requested()) {
        return remove_temporary_download(temporary_path, CUP_ERR_INTERRUPT);
    }
    if (writer.too_large || result == CURLE_FILESIZE_EXCEEDED) {
        fprintf(stderr, "Error: download exceeded the configured size limit: '%s'.\n", url);
        return remove_temporary_download(temporary_path,
            CUP_ERR_DOWNLOAD_TOO_LARGE);
    }

    if (writer.write_failed) {
        fprintf(stderr, "Error: failed to write downloaded data for '%s'.\n",
            url);
        return remove_temporary_download(temporary_path, CUP_ERR_FILESYSTEM);
    }
    if (result != CURLE_OK || info_result != CURLE_OK ||
        response_code != 200) {
        fprintf(stderr, "Error: failed to download '%s'", url);
        if (info_result == CURLE_OK && response_code > 0) {
            fprintf(stderr, " (HTTP %ld)", response_code);
        }
        if (error_buffer[0] != '\0') {
            fprintf(stderr, ": %s", error_buffer);
        }
        fputs(".\n", stderr);
        if (result == CURLE_OPERATION_TIMEDOUT) {
            err = CUP_ERR_TIMEOUT;
        } else if (result == CURLE_PEER_FAILED_VERIFICATION ||
            result == CURLE_SSL_CONNECT_ERROR ||
            result == CURLE_SSL_CERTPROBLEM ||
            result == CURLE_SSL_CACERT_BADFILE) {
            err = CUP_ERR_TLS;
        } else {
            err = CUP_ERR_FETCH;
        }
        return remove_temporary_download(temporary_path, err);
    }
    if (sync_err != CUP_OK || close_status != 0) {
        fprintf(stderr, "Error: failed to commit downloaded data for '%s'.\n",
            url);
        return remove_temporary_download(temporary_path, CUP_ERR_FILESYSTEM);
    }

    err = validate_download(temporary_path, validation);
    if (err != CUP_OK) {
        return remove_temporary_download(temporary_path, err);
    }

    err = commit_download(temporary_path, destination);
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        return remove_temporary_download(temporary_path, err);
    }
    return err;
}

static CupError build_checksum_cache_path(const char *archive_path,
    char *checksum_path, size_t size) {
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

static const char *archive_filename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static CupError verify_cached_archive(const char *checksum_path,
    const char *archive_path, int *valid) {
    CupError err;
    int archive_valid;
    int checksum_matches;

    *valid = 0;
    err = package_archive_is_valid(archive_path, &archive_valid);
    if (err != CUP_OK || !archive_valid) {
        return err;
    }
    err = checksum_verify_file(checksum_path, archive_filename(archive_path),
        archive_path, &checksum_matches);
    if (err != CUP_OK) {
        return err;
    }
    *valid = checksum_matches;
    return CUP_OK;
}

CupError fetch_package(char *archive_path, size_t archive_path_size,
    const char *package_url, const char *checksum_url,
    const PackageIdentity *identity, const char *format,
    FetchCachePolicy cache_policy, FetchSource *source) {
    CupError err;
    char checksum_path[MAX_PATH_LEN];
    int checksum_refreshed = 0;
    int valid_archive;

    if (archive_path == NULL || archive_path_size == 0 ||
        text_is_empty(package_url) || text_is_empty(checksum_url) ||
        identity == NULL || text_is_empty(format) || source == NULL ||
        (cache_policy != FETCH_ALLOW_CACHE &&
            cache_policy != FETCH_REFRESH_CACHE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_ensure_cache_parent(identity);
    if (err != CUP_OK) {
        return err;
    }
    err = layout_build_cache_archive_path(archive_path, archive_path_size,
        identity, format);
    if (err != CUP_OK || build_checksum_cache_path(archive_path, checksum_path,
            sizeof(checksum_path)) != CUP_OK) {
        return err == CUP_OK ? CUP_ERR_BUFFER_TOO_SMALL : err;
    }

    {
        SystemPathKind checksum_kind;

        err = system_get_path_kind(checksum_path, &checksum_kind);
        if (err != CUP_OK) {
            return err;
        }
        if (cache_policy == FETCH_REFRESH_CACHE ||
            checksum_kind == SYSTEM_PATH_MISSING) {
            err = fetch_file(checksum_url, checksum_path,
                FETCH_VALIDATE_METADATA);
            if (err != CUP_OK) {
                return err;
            }
            checksum_refreshed = 1;
        } else if (checksum_kind != SYSTEM_PATH_REGULAR_FILE) {
            return CUP_ERR_FILESYSTEM;
        }
    }
    err = checksum_find_expected(checksum_path, archive_filename(archive_path),
        (char[SHA256_HEX_LENGTH + 1]){0}, SHA256_HEX_LENGTH + 1);
    if (err != CUP_OK && cache_policy == FETCH_ALLOW_CACHE) {
        CupError refresh_err = fetch_file(checksum_url, checksum_path,
            FETCH_VALIDATE_METADATA);

        if (refresh_err != CUP_OK) {
            return refresh_err;
        }
        checksum_refreshed = 1;
        err = checksum_find_expected(checksum_path,
            archive_filename(archive_path),
            (char[SHA256_HEX_LENGTH + 1]){0},
            SHA256_HEX_LENGTH + 1);
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: package checksum metadata has no unique entry "
            "for '%s'.\n", archive_filename(archive_path));
        return CUP_ERR_VALIDATION;
    }

    if (cache_policy == FETCH_ALLOW_CACHE) {
        err = verify_cached_archive(checksum_path, archive_path, &valid_archive);
        if (err == CUP_OK && valid_archive) {
            *source = FETCH_SOURCE_CACHE;
            return CUP_OK;
        }
        if (err == CUP_OK && !valid_archive && !checksum_refreshed) {
            err = fetch_file(checksum_url, checksum_path,
                FETCH_VALIDATE_METADATA);
            if (err != CUP_OK) {
                return err;
            }
            checksum_refreshed = 1;
            err = checksum_find_expected(checksum_path,
                archive_filename(archive_path),
                (char[SHA256_HEX_LENGTH + 1]){0},
                SHA256_HEX_LENGTH + 1);
            if (err != CUP_OK) {
                return CUP_ERR_VALIDATION;
            }
            err = verify_cached_archive(checksum_path, archive_path,
                &valid_archive);
            if (err == CUP_OK && valid_archive) {
                *source = FETCH_SOURCE_CACHE;
                return CUP_OK;
            }
        }
        if (err != CUP_OK && err != CUP_ERR_FILESYSTEM &&
            err != CUP_ERR_ARCHIVE && err != CUP_ERR_VALIDATION) {
            return err;
        }
        err = fetch_discard_cached_package(archive_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = fetch_file(package_url, archive_path, FETCH_VALIDATE_ARCHIVE);
    if (err != CUP_OK) {
        return err;
    }
    err = verify_cached_archive(checksum_path, archive_path, &valid_archive);
    if ((err != CUP_OK || !valid_archive) && !checksum_refreshed) {
        CupError refresh_err = fetch_file(checksum_url, checksum_path,
            FETCH_VALIDATE_METADATA);

        if (refresh_err == CUP_OK &&
            checksum_find_expected(checksum_path,
                archive_filename(archive_path),
                (char[SHA256_HEX_LENGTH + 1]){0},
                SHA256_HEX_LENGTH + 1) == CUP_OK) {
            err = verify_cached_archive(checksum_path, archive_path,
                &valid_archive);
        } else {
            err = refresh_err == CUP_OK ? CUP_ERR_VALIDATION : refresh_err;
        }
    }
    if (err != CUP_OK || !valid_archive) {
        CupError discard_err = fetch_discard_cached_package(archive_path);
        fprintf(stderr, "Error: downloaded package failed SHA-256 verification.\n");
        return discard_err == CUP_OK
            ? (err == CUP_OK ? CUP_ERR_VALIDATION : err)
            : discard_err;
    }

    *source = FETCH_SOURCE_NETWORK;
    return CUP_OK;
}

CupError fetch_discard_cached_package(const char *archive_path) {
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
