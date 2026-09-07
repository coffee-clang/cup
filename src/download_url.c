/*
 * Owns release-test URL syntax delegation and the deliberately narrow CUP trust policy.
 */

#include "download.h"

#include <curl/urlapi.h>
#include <stdlib.h>
#include <string.h>

/* libcurl owns URL syntax and decomposition. CUP keeps the test-only transport override deliberately
 * small: explicit opt-in, HTTP, 127.0.0.1, an explicit non-zero port and a plain path. */
static CupError normalize_insecure_loopback_url(const char *url, char *normalized, size_t size) {
    CURLU *parsed = NULL;
    CURLUcode result;
    char *scheme = NULL;
    char *host = NULL;
    char *port = NULL;
    char *canonical = NULL;
    CupError err = CUP_ERR_INVALID_INPUT;
    size_t length;

    if (url == NULL || strpbrk(url, "?#\\") != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    parsed = curl_url();
    if (parsed == NULL) {
        return CUP_ERR_TEMPORARY;
    }
    result = curl_url_set(parsed, CURLUPART_URL, url, CURLU_DISALLOW_USER | CURLU_PATH_AS_IS);
    if (result != CURLUE_OK) {
        err = result == CURLUE_OUT_OF_MEMORY ? CUP_ERR_TEMPORARY : CUP_ERR_INVALID_INPUT;
        goto cleanup;
    }
    result = curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0);
    if (result != CURLUE_OK || strcmp(scheme, "http") != 0) {
        goto cleanup;
    }
    result = curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    if (result != CURLUE_OK || strcmp(host, "127.0.0.1") != 0) {
        goto cleanup;
    }
    result = curl_url_get(parsed, CURLUPART_PORT, &port, 0);
    if (result != CURLUE_OK || strcmp(port, "0") == 0) {
        goto cleanup;
    }

    if (normalized != NULL) {
        result = curl_url_get(parsed, CURLUPART_URL, &canonical, 0);
        if (result != CURLUE_OK) {
            err = result == CURLUE_OUT_OF_MEMORY ? CUP_ERR_TEMPORARY : CUP_ERR_INVALID_INPUT;
            goto cleanup;
        }
        length = strlen(canonical);
        while (length > 0 && canonical[length - 1] == '/') {
            length--;
        }
        if (length == 0 || length >= size) {
            err = CUP_ERR_BUFFER_TOO_SMALL;
            goto cleanup;
        }
        memcpy(normalized, canonical, length);
        normalized[length] = '\0';
    }
    err = CUP_OK;

cleanup:
    curl_free(canonical);
    curl_free(port);
    curl_free(host);
    curl_free(scheme);
    curl_url_cleanup(parsed);
    return err;
}

int download_insecure_loopback_is_allowed(const char *url) {
    const char *allow = getenv("CUP_INSTALL_ALLOW_INSECURE");

    return allow != NULL && strcmp(allow, "1") == 0 &&
           normalize_insecure_loopback_url(url, NULL, 0) == CUP_OK;
}

CupError download_copy_release_base_override(char *base, size_t size) {
    const char *value = getenv("CUP_INSTALL_BASE_URL");
    const char *allow = getenv("CUP_INSTALL_ALLOW_INSECURE");

    if (base == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    base[0] = '\0';
    if (value == NULL || value[0] == '\0') {
        return CUP_ERR_NOT_AVAILABLE;
    }
    if (allow == NULL || strcmp(allow, "1") != 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return normalize_insecure_loopback_url(value, base, size);
}
