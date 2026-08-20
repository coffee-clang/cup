/*
 * Opens only cup-supported archive formats and verifies the detected format/filter stack during
 * the actual extraction pass.
 */

#include "package_archive.h"

#include <archive.h>

static CupError configure_reader(struct archive **reader) {
    struct archive *archive_reader;
    int status;

    if (reader == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *reader = NULL;
    archive_reader = archive_read_new();
    if (archive_reader == NULL) {
        return CUP_ERR_ARCHIVE;
    }

    status = archive_read_support_filter_none(archive_reader);
    if (status == ARCHIVE_OK) {
        status = archive_read_support_filter_gzip(archive_reader);
    }
    if (status == ARCHIVE_OK) {
        status = archive_read_support_filter_xz(archive_reader);
    }
    if (status == ARCHIVE_OK) {
        status = archive_read_support_format_tar(archive_reader);
    }
    if (status == ARCHIVE_OK) {
        status = archive_read_support_format_zip(archive_reader);
    }
    if (status != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        return CUP_ERR_ARCHIVE;
    }

    *reader = archive_reader;
    return CUP_OK;
}

CupError package_archive_open_stream(struct archive **reader, FILE *file) {
    struct archive *archive_reader = NULL;
    CupError err;

    if (reader == NULL || file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *reader = NULL;
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    err = configure_reader(&archive_reader);
    if (err != CUP_OK) {
        return err;
    }
    if (archive_read_open_FILE(archive_reader, file) != ARCHIVE_OK) {
        archive_read_free(archive_reader);
        clearerr(file);
        (void)fseek(file, 0, SEEK_SET);
        return CUP_ERR_ARCHIVE;
    }
    *reader = archive_reader;
    return CUP_OK;
}

static int reader_is_tar(struct archive *reader) {
    return (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_TAR;
}

int package_archive_reader_matches_format(struct archive *reader,
                                          PackageArchiveFormat expected_format) {
    int archive_format_code;
    int filter_code;

    if (reader == NULL) {
        return 0;
    }

    archive_format_code = archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK;
    filter_code = archive_filter_code(reader, 0);

    if (expected_format == PACKAGE_ARCHIVE_FORMAT_ANY) {
        return (reader_is_tar(reader) && archive_filter_count(reader) == 2 &&
                archive_filter_code(reader, 1) == ARCHIVE_FILTER_NONE &&
                (filter_code == ARCHIVE_FILTER_XZ || filter_code == ARCHIVE_FILTER_GZIP)) ||
               (archive_format_code == ARCHIVE_FORMAT_ZIP &&
                archive_filter_count(reader) == 1 && filter_code == ARCHIVE_FILTER_NONE);
    }
    if (expected_format == PACKAGE_ARCHIVE_FORMAT_TAR_XZ) {
        return reader_is_tar(reader) && archive_filter_count(reader) == 2 &&
               filter_code == ARCHIVE_FILTER_XZ &&
               archive_filter_code(reader, 1) == ARCHIVE_FILTER_NONE;
    }
    if (expected_format == PACKAGE_ARCHIVE_FORMAT_TAR_GZ) {
        return reader_is_tar(reader) && archive_filter_count(reader) == 2 &&
               filter_code == ARCHIVE_FILTER_GZIP &&
               archive_filter_code(reader, 1) == ARCHIVE_FILTER_NONE;
    }
    if (expected_format == PACKAGE_ARCHIVE_FORMAT_ZIP) {
        return archive_format_code == ARCHIVE_FORMAT_ZIP && archive_filter_count(reader) == 1 &&
               filter_code == ARCHIVE_FILTER_NONE;
    }

    return 0;
}
