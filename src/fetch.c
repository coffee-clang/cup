#include "fetch.h"

#include "ca_bundle.h"
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

// TLS CONFIGURATION
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

// CURL CALLBACKS
static size_t write_file_callback(void *ptr, size_t size, size_t count, void *userdata) {
    FILE *file = userdata;
    size_t bytes;

    if (file == NULL || (size != 0 && count > (size_t)-1 / size)) {
        return 0;
    }

    bytes = size * count;
    return fwrite(ptr, 1, bytes, file);
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

// DOWNLOADS
static CupError download_file(const char *url, const char *final_path, int require_archive) {
    CURL *curl = NULL;
    CURLcode result = CURLE_OK;
    FILE *file = NULL;
    CupError err;
    char error_buffer[CURL_ERROR_SIZE];
    char parent[MAX_PATH_LEN];
    char part_path[MAX_PATH_LEN];
    long response_code = 0;
    int close_status;
    int usable;

    if (text_is_empty(url) || text_is_empty(final_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    error_buffer[0] = '\0';

    if (text_format(parent, sizeof(parent), "%s", final_path) != CUP_OK) {
        return CUP_ERR_FETCH;
    }
    {
        char *slash = strrchr(parent, '/');
        if (slash == NULL) {
            if (text_format(parent, sizeof(parent), ".") != CUP_OK) {
                return CUP_ERR_FETCH;
            }
        } else if (slash == parent) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    }

    initialize_tls_runtime();

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return CUP_ERR_FETCH;
    }

    if (system_create_temp_file(parent, "download", part_path,
        sizeof(part_path), &file) != CUP_OK) {
        curl_global_cleanup();
        return CUP_ERR_FETCH;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        fclose(file);
        system_remove_file(part_path);
        curl_global_cleanup();
        return CUP_ERR_FETCH;
    }

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
    SETOPT(curl, CURLOPT_WRITEDATA, file);
    SETOPT(curl, CURLOPT_NOPROGRESS, 0L);
    SETOPT(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

cleanup:
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }

    err = file != NULL ? system_sync_file(file) : CUP_ERR_FETCH;
    close_status = file != NULL ? fclose(file) : -1;
    curl_global_cleanup();

    if (result == CURLE_ABORTED_BY_CALLBACK && interrupt_requested()) {
        system_remove_file(part_path);
        return CUP_ERR_INTERRUPT;
    }

    if (result != CURLE_OK || response_code >= 400 ||
        err != CUP_OK || close_status != 0) {
        system_remove_file(part_path);
        fprintf(stderr, "Error: failed to download package from '%s'%s%s.\n",
            url, error_buffer[0] ? ": " : "",
            error_buffer[0] ? error_buffer : "");
        return CUP_ERR_FETCH;
    }

    if (require_archive) {
        err = package_archive_is_usable(part_path, &usable);
        if (err != CUP_OK || !usable) {
            system_remove_file(part_path);
            return CUP_ERR_ARCHIVE;
        }
    }

    system_set_read_only(final_path, 0);
    system_remove_file(final_path);

    {
        SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

        err = system_move_path(part_path, final_path, &commit_state);
        if (err != CUP_OK) {
            if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
                system_remove_file(part_path);
            }
            return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : CUP_ERR_FETCH;
        }
    }

    return CUP_OK;
}

CupError fetch_package(char *archive_path, size_t archive_path_size,
    const char *package_url, const PackageIdentity *identity,
    const char *format, int force_download) {
    CupError err;
    int usable;

    if (archive_path == NULL || archive_path_size == 0 ||
        text_is_empty(package_url) || identity == NULL || text_is_empty(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_ensure_cache_parent(identity);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_build_cache_archive_path(archive_path,
        archive_path_size, identity, format);
    if (err != CUP_OK) {
        return err;
    }

    if (force_download) {
        system_set_read_only(archive_path, 0);
        system_remove_file(archive_path);
        usable = 0;
    } else {
        err = package_archive_is_usable(archive_path, &usable);
        if (err != CUP_OK) {
            return err;
        }
    }

    if (!usable) {
        return download_file(package_url, archive_path, 1);
    }

    return CUP_OK;
}

CupError fetch_resource(const char *url, const char *destination) {
    return download_file(url, destination, 0);
}
