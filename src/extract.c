#include "extract.h"
#include "constants.h"
#include "util.h"

#include <archive.h>
#include <archive_entry.h>

#include <stdio.h>
#include <string.h>

static int path_has_parent_reference(const char *path) {
    const char *start;
    const char *slash;
    size_t len;

    if (path == NULL) {
        return 1;
    }

    start = path;

    while (*start != '\0') {
        slash = strchr(start, '/');

        if (slash == NULL) {
            len = strlen(start);
        } else {
            len = (size_t)(slash - start);
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return 1;
        }

        if (slash == NULL) {
            break;
        }

        start = slash + 1;
    }

    return 0;
}

static const char *strip_first_path_component(const char *path) {
    const char *slash;

    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    slash = strchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }

    return slash + 1;
}

static CupError build_output_path(char *buffer, size_t size, const char *root, const char *entry_path) {
    CupError err;

    if (buffer == NULL || root == NULL || entry_path == NULL ||
        size == 0 || root[0] == '\0' || entry_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (entry_path[0] == '/') {
        fprintf(stderr, "Error: archive contains an absolute path '%s'.\n", entry_path);
        return CUP_ERR_INSTALL;
    }

    if (path_has_parent_reference(entry_path)) {
        fprintf(stderr, "Error: archive contains an unsafe path '%s'.\n", entry_path);
        return CUP_ERR_INSTALL;
    }

    err = checked_snprintf(buffer, size, "%s/%s", root, entry_path);
    if (err != CUP_OK) {
        return CUP_ERR_INSTALL;
    }

    return CUP_OK;
}

static CupError rewrite_entry_paths(const char *tmp_path, struct archive_entry *entry, int *should_skip) {
    CupError err;
    const char *entry_path;
    const char *stripped_path;
    const char *hardlink_path;
    const char *stripped_hardlink;
    char output_hardlink[MAX_PATH_LEN];
    char output_path[MAX_PATH_LEN];

    if (tmp_path == NULL || entry == NULL || should_skip == NULL ||
        tmp_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    *should_skip = 0;

    entry_path = archive_entry_pathname(entry);
    if (entry_path == NULL || entry_path[0] == '\0') {
        fprintf(stderr, "Error: archive entry has no pathname.\n");
        return CUP_ERR_INSTALL;
    }

    stripped_path = strip_first_path_component(entry_path);
    if (stripped_path == NULL) {
        *should_skip = 1;
        return CUP_OK;
    }

    err = build_output_path(output_path, sizeof(output_path), tmp_path, stripped_path);
    if (err != CUP_OK) {
        return err;
    }

    archive_entry_set_pathname(entry, output_path);

    hardlink_path = archive_entry_hardlink(entry);
    if (hardlink_path != NULL) {

        stripped_hardlink = strip_first_path_component(hardlink_path);
        if (stripped_hardlink == NULL) {
            fprintf(stderr, "Error: archive contains invalid hardlink target '%s'.\n", hardlink_path);
            return CUP_ERR_INSTALL;
        }

        err = build_output_path(output_hardlink, sizeof(output_hardlink), tmp_path, stripped_hardlink);
        if (err != CUP_OK) {
            return err;
        }

        archive_entry_set_hardlink(entry, output_hardlink);
    }

    return CUP_OK;
}


static CupError copy_archive_data(struct archive *reader, struct archive *writer) {
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int status;

    if (reader == NULL || writer == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (;;) {
        status = archive_read_data_block(reader, &buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while reading archive data: %s.\n", archive_error_string(reader));
            return CUP_ERR_INSTALL;
        }

        status = archive_write_data_block(writer, buffer, size, offset);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while writing archive data: %s.\n", archive_error_string(writer));
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
    int should_skip;
    int status;

    if (archive_path == NULL || tmp_path == NULL || 
        archive_path[0] == '\0' || tmp_path[0] == '\0') {
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

    status = archive_read_support_filter_all(reader);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: could not enable archive filters: %s", archive_error_string(reader));
        cleanup_archives(reader, writer);
        return CUP_ERR_INSTALL;
    }
    status = archive_read_support_format_all(reader);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: could not enable archive formats: %s", archive_error_string(reader));
        cleanup_archives(reader, writer);
        return CUP_ERR_INSTALL;
    }

    status = archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME |
        ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: could not configure archive writer: %s", archive_error_string(writer));
        cleanup_archives(reader, writer);
        return CUP_ERR_INSTALL;
    }

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

        err = rewrite_entry_paths(tmp_path, entry, &should_skip);
        if (err != CUP_OK) {
            cleanup_archives(reader, writer);
            return err;
        }

        if (should_skip) {
            continue;
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