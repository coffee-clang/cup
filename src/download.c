/*
 * Performs bounded HTTPS downloads with transfer limits, embedded trust and atomic destination
 * replacement.
 */

#include "download.h"

#include "ca_bundle.h"
#include "constants.h"
#include "interrupt.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <curl/curl.h>
#if defined(CUP_USE_OPENSSL_INIT)
#include <openssl/ssl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TLS initialization and transfer limits. */
static CupError initialize_tls_runtime(void) {
#if defined(CUP_USE_OPENSSL_INIT)
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) != 1) {
        fprintf(stderr, "Error: could not initialize the TLS runtime.\n");
        return CUP_ERR_FETCH;
    }
#endif
    return CUP_OK;
}

static CupError configure_tls_trust(CURL *curl) {
#if defined(CUP_USE_EMBEDDED_CA_BUNDLE)
    struct curl_blob ca_blob;
    CURLcode result;

    /* libcurl models blob data as mutable even with CURL_BLOB_NOCOPY;
     * the transfer API does not modify this embedded read-only bundle. */
    ca_blob.data = (void *)cup_ca_bundle;
    ca_blob.len = cup_ca_bundle_len;
    ca_blob.flags = CURL_BLOB_NOCOPY;

    result = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
    if (result != CURLE_OK) {
        fprintf(stderr,
                "Error: could not configure embedded CA bundle: %s.\n",
                curl_easy_strerror(result));
        return CUP_ERR_FETCH;
    }
#else
    (void)curl;
#endif

    return CUP_OK;
}

static curl_off_t validation_limit(DownloadValidation validation) {
    switch (validation) {
        case DOWNLOAD_VALIDATE_METADATA:
        case DOWNLOAD_VALIDATE_NONEMPTY:
            return (curl_off_t)MAX_METADATA_DOWNLOAD_BYTES;
        case DOWNLOAD_VALIDATE_BINARY:
            return (curl_off_t)MAX_BINARY_DOWNLOAD_BYTES;
        case DOWNLOAD_VALIDATE_ARCHIVE:
            return (curl_off_t)MAX_PACKAGE_DOWNLOAD_BYTES;
        default:
            return 0;
    }
}

static long validation_timeout(DownloadValidation validation) {
    return validation == DOWNLOAD_VALIDATE_ARCHIVE ? 7200L : 300L;
}

static int progress_callback(void *userdata,
                             curl_off_t download_total,
                             curl_off_t downloaded,
                             curl_off_t upload_total,
                             curl_off_t uploaded) {
    (void)userdata;
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;

    return interrupt_requested() ? 1 : 0;
}

#define SETOPT(handle, option, value) \
    do { \
        CURLcode setopt_result = curl_easy_setopt((handle), (option), (value)); \
        if (setopt_result != CURLE_OK) { \
            fprintf(stderr, \
                    "Error: could not configure libcurl option %s: %s.\n", \
                    #option, \
                    curl_easy_strerror(setopt_result)); \
            result = setopt_result; \
            goto cleanup; \
        } \
    } while (0)

static CURLcode configure_transfer(CURL *curl,
                                   const char *url,
                                   DownloadValidation validation,
                                   FILE *file,
                                   char *error_buffer) {
    CURLcode result = CURLE_OK;
    int insecure_loopback;

    if (configure_tls_trust(curl) != CUP_OK) {
        return CURLE_FAILED_INIT;
    }
    insecure_loopback = download_insecure_loopback_is_allowed(url);

    SETOPT(curl, CURLOPT_ERRORBUFFER, error_buffer);
    SETOPT(curl, CURLOPT_URL, url);
    SETOPT(curl, CURLOPT_FOLLOWLOCATION, insecure_loopback ? 0L : 1L);
    SETOPT(curl, CURLOPT_MAXREDIRS, insecure_loopback ? 0L : 10L);
    SETOPT(curl, CURLOPT_FAILONERROR, 1L);
    SETOPT(curl, CURLOPT_USERAGENT, "cup");
    SETOPT(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SETOPT(curl, CURLOPT_ACCEPT_ENCODING, "");
    SETOPT(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    SETOPT(curl, CURLOPT_TIMEOUT, validation_timeout(validation));
    SETOPT(curl, CURLOPT_MAXFILESIZE_LARGE, validation_limit(validation));
    SETOPT(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    SETOPT(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    SETOPT(curl, CURLOPT_PROTOCOLS_STR, insecure_loopback ? "http" : "https");
    SETOPT(curl, CURLOPT_REDIR_PROTOCOLS_STR, insecure_loopback ? "http" : "https");
    SETOPT(curl, CURLOPT_WRITEDATA, file);
    SETOPT(curl, CURLOPT_NOPROGRESS, 0L);
    SETOPT(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

cleanup:
    return result;
}

/* Atomic destination preparation. */
static CupError remove_temporary_download(const char *path, CupError original_error) {
    return system_remove_file(path) == CUP_OK ? original_error : CUP_ERR_TEMPORARY;
}


static CupError classify_transfer_result(const char *url,
                                         const char *temporary_path,
                                         CURLcode result,
                                         CURLcode metadata_result,
                                         long response_code,
                                         const char *error_buffer) {
    CupError err;

    if (result == CURLE_ABORTED_BY_CALLBACK && interrupt_requested()) {
        return remove_temporary_download(temporary_path, CUP_ERR_INTERRUPT);
    }
    if (result == CURLE_FILESIZE_EXCEEDED) {
        fprintf(stderr, "Error: download exceeded the configured size limit: '%s'.\n", url);
        return remove_temporary_download(temporary_path, CUP_ERR_DOWNLOAD_TOO_LARGE);
    }
    if (result == CURLE_WRITE_ERROR) {
        fprintf(stderr, "Error: failed to write downloaded data for '%s'.\n", url);
        return remove_temporary_download(temporary_path, CUP_ERR_FILESYSTEM);
    }
    if (result == CURLE_OK && metadata_result == CURLE_OK && response_code == 200) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: failed to download '%s'", url);
    if (metadata_result == CURLE_OK && response_code > 0) {
        fprintf(stderr, " (HTTP %ld)", response_code);
    }
    if (error_buffer[0] != '\0') {
        fprintf(stderr, ": %s", error_buffer);
    }
    fputs(".\n", stderr);

    if (result == CURLE_OPERATION_TIMEDOUT) {
        err = CUP_ERR_TIMEOUT;
    } else if (result == CURLE_PEER_FAILED_VERIFICATION ||
               result == CURLE_SSL_CONNECT_ERROR || result == CURLE_SSL_CERTPROBLEM ||
               result == CURLE_SSL_CACERT_BADFILE) {
        err = CUP_ERR_TLS;
    } else {
        err = CUP_ERR_FETCH;
    }
    return remove_temporary_download(temporary_path, err);
}

/* Content-class validation. Each asset type has a bounded parser rather than relying on a
 * successful HTTP response alone. */
static CupError validate_download(const char *path, DownloadValidation validation) {
    CupError err;
    long long size;
    int is_regular_file;

    err = system_is_regular_file(path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular_file) {
        return validation == DOWNLOAD_VALIDATE_ARCHIVE ? CUP_ERR_ARCHIVE : CUP_ERR_FETCH;
    }

    err = system_file_size(path, &size);
    if (err != CUP_OK) {
        return err;
    }
    if (size <= 0) {
        fprintf(stderr, "Error: downloaded resource is empty.\n");
        return validation == DOWNLOAD_VALIDATE_ARCHIVE ? CUP_ERR_ARCHIVE : CUP_ERR_FETCH;
    }

    /* Archive syntax is intentionally not parsed before an authenticated digest exists. */
    return CUP_OK;
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
        fprintf(stderr, "Error: download destination '%s' is not a regular file.\n", path);
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

static CupError commit_download(const char *temporary_path, const char *destination) {
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

/* Atomic download pipeline. Data is written to an exclusive temporary file, validated, synced and
 * then committed. */
CupError download_file_checked(const char *url,
                               const char *destination,
                               DownloadValidation validation,
                               DownloadValidator validator,
                               void *validator_data) {
    CURL *curl = NULL;
    CURLcode result = CURLE_OK;
    CURLcode package_metadata_result = CURLE_OK;
    FILE *file = NULL;
    CupError sync_err = CUP_OK;
    CupError err;
    char error_buffer[CURL_ERROR_SIZE];
    char parent[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN] = "";
    long response_code = 0;
    int close_status = 0;

    if (text_is_empty(url) || text_is_empty(destination) ||
        (validation != DOWNLOAD_VALIDATE_NONEMPTY && validation != DOWNLOAD_VALIDATE_METADATA &&
         validation != DOWNLOAD_VALIDATE_BINARY && validation != DOWNLOAD_VALIDATE_ARCHIVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Create the transfer beside the destination so the final replace stays on one filesystem. */
    err = path_parent(parent, sizeof(parent), destination);
    if (err != CUP_OK) {
        return err;
    }

    err = initialize_tls_runtime();
    if (err != CUP_OK) {
        return err;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return CUP_ERR_FETCH;
    }

    err =
        system_create_temp_file(parent, "download", temporary_path, sizeof(temporary_path), &file);
    if (err != CUP_OK) {
        curl_global_cleanup();
        return err;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        int close_failed = fclose(file) != 0;
        CupError cleanup_error = system_remove_file(temporary_path);

        curl_global_cleanup();
        return close_failed || cleanup_error != CUP_OK ? CUP_ERR_TEMPORARY : CUP_ERR_FETCH;
    }

    /* Apply protocol, trust, timeout and size policy before the first network byte is accepted. */
    error_buffer[0] = '\0';
    result = configure_transfer(curl, url, validation, file, error_buffer);
    if (result == CURLE_OK) {
        result = curl_easy_perform(curl);
        package_metadata_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    /* Classify the transfer before issuing durability I/O for bytes that will be rejected. */
    curl_easy_cleanup(curl);
    if (!(result == CURLE_OK && package_metadata_result == CURLE_OK && response_code == 200)) {
        close_status = fclose(file);
        file = NULL;
        curl_global_cleanup();
        err = classify_transfer_result(url,
                                       temporary_path,
                                       result,
                                       package_metadata_result,
                                       response_code,
                                       error_buffer);
        return close_status == 0 ? err : CUP_ERR_FILESYSTEM;
    }

    sync_err = system_sync_file(file);
    close_status = fclose(file);
    file = NULL;
    curl_global_cleanup();
    if (sync_err != CUP_OK || close_status != 0) {
        fprintf(stderr, "Error: failed to commit downloaded data for '%s'.\n", url);
        return remove_temporary_download(temporary_path, CUP_ERR_FILESYSTEM);
    }

    /* Content validation happens before the atomic destination replacement. */
    err = validate_download(temporary_path, validation);
    if (err == CUP_OK && validator != NULL) {
        err = validator(temporary_path, validator_data);
    }
    if (err != CUP_OK) {
        return remove_temporary_download(temporary_path, err);
    }

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return remove_temporary_download(temporary_path, err);
    }

    err = commit_download(temporary_path, destination);
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        return remove_temporary_download(temporary_path, err);
    }
    return err;
}

CupError download_file(const char *url, const char *destination, DownloadValidation validation) {
    return download_file_checked(url, destination, validation, NULL, NULL);
}
