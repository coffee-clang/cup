#include "fetch.h"
#include "filesystem.h"
#include "manifest.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>

#include <curl/curl.h>

static size_t write_file_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file;

    if (userdata == NULL) {
        return 0;
    }

    file = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, file);
}

static CupError download_package(const char *url, const char *dst_path) {
    CupError err;
    CURL *curl;
    CURLcode res;
    FILE *file;
    int status;
    int usable;

    if (url == NULL || dst_path == NULL ||
        url[0] == '\0' || dst_path[0] == '\0') {
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
        remove(dst_path);
        curl_global_cleanup();
        fprintf(stderr, "Error: could not create libcurl handle.\n");
        return CUP_ERR_FETCH;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cup");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    status = fclose(file);
    file = NULL;

    curl_global_cleanup();

    if (res != CURLE_OK) {
        remove(dst_path);
        fprintf(stderr, "Error: failed to download package from '%s': %s.\n", url, curl_easy_strerror(res));
        return CUP_ERR_FETCH;
    }

    if (status != 0) {
        remove(dst_path);
        fprintf(stderr, "Error: could not finalize downloaded file '%s'.\n", dst_path);
        return CUP_ERR_FETCH;
    }

    err = archive_is_usable(dst_path, &usable);
    if (err != CUP_OK) {
        remove(dst_path);
        return CUP_ERR_FETCH;
    }

    if (!usable) {
        remove(dst_path);
        fprintf(stderr, "Error: downloaded package is empty or was not created correctly.\n");
        return CUP_ERR_FETCH;
    }

    return CUP_OK;
}

CupError fetch_package(char *buffer, size_t size, const char *component, const char *tool, const char *resolved_release, const char *archive_format) {
    CupError err;
    char archive_path[MAX_PATH_LEN];
    char package_url[MAX_MANIFEST_URL_LEN];
    int usable;

    if (buffer == NULL || component == NULL || tool == NULL || resolved_release == NULL || archive_format == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || resolved_release[0] == '\0' || archive_format[0] == '\0') {
        fprintf(stderr, "Error: invalid package fetch arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cache_package_dirs(component, tool, resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_cache_archive_path(archive_path, sizeof(archive_path), component, tool, resolved_release, archive_format);
    if (err != CUP_OK) {
        return err;
    }

    err = archive_is_usable(archive_path, &usable);
    if (err != CUP_OK) {
        return err;
    }

    if (!usable) {
        err = build_package_url_from_manifest(package_url, sizeof(package_url), component, tool, resolved_release, archive_format);
        if (err != CUP_OK) {
            return err;
        }

        err = download_package(package_url, archive_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = checked_snprintf(buffer, size, "%s", archive_path);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}