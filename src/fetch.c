#include "fetch.h"
#include "fs.h"
#include "manifest.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

#include <curl/curl.h>

static size_t write_file_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, file);
}

CupError download_package(const char *url, const char *dst_path) {
    CURL *curl;
    CURLcode res;
    FILE *file;
    CupError err;

    if (url == NULL || dst_path == NULL) {
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    fclose(file);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        remove(dst_path);
        fprintf(stderr, "Error: failed to download package from '%s': %s.\n", url, curl_easy_strerror(res));
        return CUP_ERR_FETCH;
    }

    err = archive_exists(dst_path, &(int){0});
    (void)err;

    return CUP_OK;
}

CupError fetch_package(char *buffer, size_t size, const char *component, const char *tool, const char *resolved_release, const char *archive_format) {
    CupError err;
    char archive_path[MAX_PATH_LEN];
    char package_url[MAX_PATH_LEN];
    int exists;

    if (buffer == NULL || component == NULL || tool == NULL || resolved_release == NULL || archive_format == NULL) {
        fprintf(stderr, "Error: invalid package fetch arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cache_package_dirs(component, tool, resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = build_cache_archive_path(archive_path, sizeof(archive_path), component, tool, resolved_release, archive_format);
    if (err != CUP_OK) {
        return CUP_ERR_FETCH;
    }

    err = archive_exists(archive_path, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (!exists) {
        err = build_package_url_from_manifest(package_url, sizeof(package_url), component, tool, resolved_release, archive_format);
        if (err != CUP_OK) {
            return CUP_ERR_FETCH;
        }

        err = download_package(package_url, archive_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = checked_snprintf(buffer, size, "%s", archive_path);
    if (err != CUP_OK) {
        return CUP_ERR_FETCH;
    }

    return CUP_OK;
}