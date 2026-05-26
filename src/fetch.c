#include "fetch.h"

#include "constants.h"
#include "filesystem.h"
#include "interrupt.h"
#include "package_archive.h"
#include "system.h"
#include "util.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CALLBACKS
static size_t write_file_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file;

    if (userdata == NULL) {
        return 0;
    }

    file = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, file);
}

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    return interrupt_requested() ? 1 : 0;
}

// URL HELPERS
static CupError get_archive_name(char *buffer, size_t size, const char *url) {
    CupError err;
    const char *slash;
    const char *start;
    char temp[MAX_PATH_LEN];
    char *query;
    char *fragment;

    if (buffer == NULL || size == 0 || is_empty_string(url)) {
        return CUP_ERR_INVALID_INPUT;
    }

    slash = strrchr(url, '/');
    if (slash == NULL || slash[1] == '\0') {
        return CUP_ERR_FETCH;
    }

    start = slash + 1;

    err = checked_snprintf(temp, sizeof(temp), "%s", start);
    if (err != CUP_OK) {
        return err;
    }

    query = strchr(temp, '?');
    if (query != NULL) {
        *query = '\0';
    }

    fragment = strchr(temp, '#');
    if (fragment != NULL) {
        *fragment = '\0';
    }

    if (temp[0] == '\0') {
        return CUP_ERR_FETCH;
    }

    err = checked_snprintf(buffer, size, "%s", temp);
    return err;
}

// DOWNLOAD
static CupError download_package(const char *url, const char *dst_path) {
    CURL *curl;
    CURLcode res;
    FILE *file;
    char error_buffer[CURL_ERROR_SIZE];
    long response_code;
    int status;
    int usable;
    CupError err;

    if (is_empty_string(url) || is_empty_string(dst_path)) {
        fprintf(stderr, "Error: invalid download arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Error: could not initialize libcurl.\n");
        return CUP_ERR_FETCH;
    }

    file = fopen(dst_path, "wb");
    if (file == NULL) {
        curl_global_cleanup();
        fprintf(stderr, "Error: could not open destination file '%s'.\n", dst_path);
        return CUP_ERR_FETCH;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        fclose(file);
        system_remove_file(dst_path);
        curl_global_cleanup();
        fprintf(stderr, "Error: could not create libcurl handle.\n");
        return CUP_ERR_FETCH;
    }

    error_buffer[0] = '\0';
    response_code = 0;

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cup");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

    res = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_easy_cleanup(curl);

    status = fclose(file);
    file = NULL;

    curl_global_cleanup();

    if (res == CURLE_ABORTED_BY_CALLBACK && interrupt_requested()) {
        system_remove_file(dst_path);
        fprintf(stderr, "\nError: download interrupted.\n");
        return CUP_ERR_INTERRUPT;
    }

    if (res != CURLE_OK) {
        system_remove_file(dst_path);

        fprintf(stderr, "Error: failed to download package from '%s': %s%s%s.\n", url, curl_easy_strerror(res),
                error_buffer[0] != '\0' ? " - " : "", error_buffer[0] != '\0' ? error_buffer : "");

        return CUP_ERR_FETCH;
    }

    if (response_code >= 400) {
        system_remove_file(dst_path);
        fprintf(stderr, "Error: failed to download package from '%s': HTTP %ld.\n", url, response_code);
        return CUP_ERR_FETCH;
    }

    if (status != 0) {
        system_remove_file(dst_path);
        fprintf(stderr, "Error: could not finalize downloaded file '%s'.\n", dst_path);
        return CUP_ERR_FETCH;
    }

    err = package_archive_is_usable(dst_path, &usable);
    if (err != CUP_OK) {
        system_remove_file(dst_path);
        return err;
    }

    if (!usable) {
        system_remove_file(dst_path);
        fprintf(stderr, "Error: downloaded package is empty or is not a readable archive.\n");
        return CUP_ERR_ARCHIVE;
    }

    return CUP_OK;
}

// PUBLIC API
CupError fetch_package(char *archive_path, size_t archive_path_size, const char *package_url, const char *component, const char *tool, const char *version) {
    CupError err;
    char archive_name[MAX_PATH_LEN];
    int usable;

    if (archive_path == NULL || archive_path_size == 0 || is_empty_string(package_url) || 
        is_empty_string(component) || is_empty_string(tool) || is_empty_string(version)) {
        fprintf(stderr, "Error: invalid package fetch arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_archive_name(archive_name, sizeof(archive_name), package_url);
    if (err != CUP_OK) {
        return err;
    }

    err = ensure_cache_package_dirs(component, tool, version);
    if (err != CUP_OK) {
        return err;
    }

    err = build_cache_archive_path(archive_path, archive_path_size, component, tool, version, archive_name);
    if (err != CUP_OK) {
        return err;
    }

    err = package_archive_is_usable(archive_path, &usable);
    if (err != CUP_OK) {
        return err;
    }

    if (!usable) {
        err = download_package(package_url, archive_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}
