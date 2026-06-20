#include "package_archive.h"

#include "system.h"
#include "util.h"

#include <archive.h>
#include <archive_entry.h>

#define ARCHIVE_READ_BLOCK_SIZE 10240

// READER
CupError package_archive_open_reader(struct archive **reader, const char *archive_path) {
    struct archive *archive_reader;
    int status;

    if (reader == NULL || is_empty_string(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *reader = NULL;

    archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return CUP_ERR_ARCHIVE;
    }

    status = archive_read_support_filter_all(archive_reader);
    if (status != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        return CUP_ERR_ARCHIVE;
    }

    status = archive_read_support_format_all(archive_reader);
    if (status != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        return CUP_ERR_ARCHIVE;
    }

    status = archive_read_open_filename(archive_reader, archive_path, ARCHIVE_READ_BLOCK_SIZE);
    if (status != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        return CUP_ERR_ARCHIVE;
    }

    *reader = archive_reader;
    return CUP_OK;
}

// CACHE VALIDATION
static CupError archive_has_header(const char *archive_path, int *has_header) {
    CupError err;
    struct archive *reader;
    struct archive_entry *entry;
    int status;

    if (has_header == NULL || is_empty_string(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *has_header = 0;

    err = package_archive_open_reader(&reader, archive_path);
    if (err != CUP_OK) {
        return CUP_OK;
    }

    status = archive_read_next_header(reader, &entry);
    if (status == ARCHIVE_OK) {
        *has_header = 1;
    }

    archive_read_close(reader);
    archive_read_free(reader);

    return CUP_OK;
}

CupError package_archive_is_usable(const char *archive_path, int *is_usable) {
    CupError err;
    int is_regular_file;
    int has_header;
    long long size;

    if (is_usable == NULL || is_empty_string(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_usable = 0;

    err = system_is_regular_file(archive_path, &is_regular_file);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!is_regular_file) {
        return CUP_OK;
    }

    err = system_file_size(archive_path, &size);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (size <= 0) {
        return CUP_OK;
    }

    err = archive_has_header(archive_path, &has_header);
    if (err != CUP_OK) {
        return err;
    }

    if (!has_header) {
        return CUP_OK;
    }

    *is_usable = 1;
    return CUP_OK;
}
