#include "archive.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <archive.h>
#include <archive_entry.h>

static CupError build_output_path(char *buffer, size_t size, const char *root, const char *entry_path) {
    if (buffer == NULL || root == NULL || entry_path == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (entry_path[0] == '/') {
        fprintf(stderr, "Error: archive contains an absolute path '%s'.\n", entry_path);
        return CUP_ERR_INSTALL;
    }

    if (strstr(entry_path, "../") != NULL || strcmp(entry_path, "..") == 0) {
        fprintf(stderr, "Error: archive contains an unsage path '%s'.\n", entry_path);
        return CUP_ERR_INSTALL;
    }

    return checked_snprintf(buffer, size, "%s/%s", root, entry_path);
}

static CupError rewrite_entry_paths(const char *tmp_path, struct archive_entry *entry) {
    CupError err;
    const char *entry_path;
    const char *hardlink_path;
    char output_path[MAX_PATH_LEN];

    if (tmp_path == NULL || entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    entry_path = archive_entry_pathname(entry);
    if (entry_path == NULL) {
        fprintf(stderr, "Error: archive entry has no pathname.\n");
        return CUP_ERR_INSTALL;
    }

    err = build_output_path(output_path, sizeof(output_path), tmp_path, entry_path);
    if (err != CUP_OK) {
        return err;
    }

    archive_entry_set_pathname(entry, output_path);

    /*
     * Hardlink targets refer to another extracted filesystem path, so they
     * must be rewritten under tmp_path as well.
     *
     * Symlink targets are left untouched intentionally, because their meaning
     * is different and they are not filesystem paths that we should blindly
     * relocate under tmp_path.
     */
    hardlink_path = archive_entry_hardlink(entry);
    if (hardlink_path != NULL) {
        char output_hardlink[MAX_PATH_LEN];

        err = build_output_path(output_hardlink, sizeof(output_hardlink),
                                tmp_path, hardlink_path);
        if (err != CUP_OK) {
            return err;
        }

        archive_entry_set_hardlink(entry, output_hardlink);
    }

    return CUP_OK;
}


static CupError copy_archive_data(struct archive *reader, struct archive *writer) {
    const void *buff;
    size_t size;
    la_int64_t offset;
    int status;

    for (;;) {
        status = archive_read_data_block(reader, &buff, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while reading archive data: %s.\n",
                    archive_error_string(reader));
            return CUP_ERR_INSTALL;
        }

        status = archive_write_data_block(writer, buff, size, offset);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while writing archive data: %s.\n",
                    archive_error_string(writer));
            return CUP_ERR_INSTALL;
        }
    }
}

static void cleanup_archives(struct archive *reader, struct archive *writer) {
    if (reader != NULL) {
        archive_read_close(reader);
        archive_read_free(reader);
    }

    if (writer != NULL) {
        archive_write_close(writer);
        archive_write_free(writer);
    }
}

CupError extract_archive_to_tmp(const char *archive_path, const char *tmp_path) {
    CupError err;
    struct archive *reader;
    struct archive *writer;
    struct archive_entry *entry;
    int status;

    if (archive_path == NULL || tmp_path == NULL) {
        fprintf(stderr, "Error: invalid archive extraction arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    reader = archive_read_new();
    if (reader == NULL) {
        fprintf(stderr, "Error: could not create libarchive reader.\n");
        return CUP_ERR_INSTALL;
    }

    writer = archive_write_disk_new();
    if (writer == NULL) {
        cleanup_archives(reader, NULL);
        fprintf(stderr, "Error: could not create libarchive writer.\n");
        return CUP_ERR_INSTALL;
    }

    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);

    archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME |
        ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_FFLAGS);

    status = archive_read_open_filename(reader, archive_path, 10240);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: failed to open archive '%s': %s.\n", archive_path, archive_error_string(reader));
        cleanup_archives(reader, writer);
        return CUP_ERR_INSTALL;
    }

    for (;;) {
        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to read archive header: %s.\n", archive_error_string(reader));
            cleanup_archives(reader, writer);
            return CUP_ERR_INSTALL;
        }

        err = rewrite_entry_paths(tmp_path, entry);
        if (err != CUP_OK) {
            cleanup_archives(reader, writer);
            return err;
        }

        status = archive_write_header(writer, entry);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to write archive entry '%s': %s.\n", archive_entry_pathname(entry), archive_error_string(writer));
            cleanup_archives(reader, writer);
            return CUP_ERR_INSTALL;
        }

        err = copy_archive_data(reader, writer);
        if (err != CUP_OK) {
            cleanup_archives(reader, writer);
            return err;
        }

        status = archive_write_finish_entry(writer);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to finalize archive entry '%s': %s.\n", archive_entry_pathname(entry), archive_error_string(writer));
            cleanup_archives(reader, writer);
            return CUP_ERR_INSTALL;
        }
    }

    cleanup_archives(reader, writer);
    return CUP_OK;
}