/*
 * Performs bounded HTTPS downloads with transfer limits, embedded trust and atomic destination
 * replacement.
 */

#include "download.h"

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

/* TLS initialization and transfer limits. */
static void initialize_tls_runtime(void) {
#if defined(CUP_USE_OPENSSL_INIT)
    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
#endif
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

typedef struct {
    FILE *file;
    curl_off_t limit;
    curl_off_t written;
    int too_large;
    int write_failed;
} DownloadWriter;

/* Transfer callbacks. Limits and interrupt checks are enforced while bytes are arriving, before
 * they reach the destination. */
static size_t write_file_callback(void *data, size_t size, size_t count, void *userdata) {
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
                                   DownloadWriter *writer,
                                   char *error_buffer) {
    CURLcode result = CURLE_OK;

    if (configure_tls_trust(curl) != CUP_OK) {
        return CURLE_FAILED_INIT;
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
    SETOPT(curl, CURLOPT_MAXFILESIZE_LARGE, writer->limit);
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
    SETOPT(curl, CURLOPT_WRITEDATA, writer);
    SETOPT(curl, CURLOPT_NOPROGRESS, 0L);
    SETOPT(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

cleanup:
    return result;
}

/* Atomic destination preparation. */
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

static CupError remove_temporary_download(const char *path, CupError original_error) {
    return system_remove_file(path) == CUP_OK ? original_error : CUP_ERR_TEMPORARY;
}


static CupError classify_transfer_result(const char *url,
                                         const char *temporary_path,
                                         CURLcode result,
                                         CURLcode metadata_result,
                                         long response_code,
                                         const char *error_buffer,
                                         const DownloadWriter *writer) {
    CupError err;

    if (result == CURLE_ABORTED_BY_CALLBACK && interrupt_requested()) {
        return remove_temporary_download(temporary_path, CUP_ERR_INTERRUPT);
    }
    if (writer->too_large || result == CURLE_FILESIZE_EXCEEDED) {
        fprintf(stderr, "Error: download exceeded the configured size limit: '%s'.\n", url);
        return remove_temporary_download(temporary_path, CUP_ERR_DOWNLOAD_TOO_LARGE);
    }
    if (writer->write_failed) {
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
    int is_valid_archive;

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

    if (validation != DOWNLOAD_VALIDATE_ARCHIVE) {
        return CUP_OK;
    }

    err = package_archive_is_valid(path, NULL, &is_valid_archive);
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
CupError download_file(const char *url, const char *destination, DownloadValidation validation) {
    CURL *curl = NULL;
    CURLcode result = CURLE_OK;
    CURLcode package_metadata_result = CURLE_OK;
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
        (validation != DOWNLOAD_VALIDATE_NONEMPTY && validation != DOWNLOAD_VALIDATE_METADATA &&
         validation != DOWNLOAD_VALIDATE_BINARY && validation != DOWNLOAD_VALIDATE_ARCHIVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Create the transfer beside the destination so the final replace stays on one filesystem. */
    err = get_parent_path(destination, parent, sizeof(parent));
    if (err != CUP_OK) {
        return err;
    }

    initialize_tls_runtime();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return CUP_ERR_FETCH;
    }

    err =
        system_create_temp_file(parent, "download", temporary_path, sizeof(temporary_path), &file);
    if (err != CUP_OK) {
        curl_global_cleanup();
        return CUP_ERR_FETCH;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        int close_failed = fclose(file) != 0;
        CupError cleanup_error = system_remove_file(temporary_path);

        curl_global_cleanup();
        return close_failed || cleanup_error != CUP_OK ? CUP_ERR_TEMPORARY : CUP_ERR_FETCH;
    }

    /* Apply protocol, trust, timeout and size policy before the first network byte is accepted. */
    writer.file = file;
    writer.limit = validation_limit(validation);
    error_buffer[0] = '\0';
    result = configure_transfer(curl, url, validation, &writer, error_buffer);
    if (result == CURLE_OK) {
        result = curl_easy_perform(curl);
        package_metadata_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    /* Close and sync the temporary file before classifying transfer or validation failures. */
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }

    if (file != NULL) {
        sync_err = system_sync_file(file);
        close_status = fclose(file);
        file = NULL;
    }
    curl_global_cleanup();

    /* Translate transport outcomes into stable CUP errors and always remove rejected data. */
    err = classify_transfer_result(url,
                                   temporary_path,
                                   result,
                                   package_metadata_result,
                                   response_code,
                                   error_buffer,
                                   &writer);
    if (err != CUP_OK) {
        return err;
    }
    if (sync_err != CUP_OK || close_status != 0) {
        fprintf(stderr, "Error: failed to commit downloaded data for '%s'.\n", url);
        return remove_temporary_download(temporary_path, CUP_ERR_FILESYSTEM);
    }

    /* Content validation happens before the atomic destination replacement. */
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
