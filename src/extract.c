#include "extract.h"

#include "constants.h"
#include "interrupt.h"
#include "package_archive.h"
#include "path.h"
#include "util.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>

// PATH REWRITING
static const char *strip_first_path_component(const char *path) {
    const char *slash;

    if (is_empty_string(path)) {
        return NULL;
    }

    slash = strchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }

    return slash + 1;
}

static CupError rewrite_entry_paths(const char *tmp_path, struct archive_entry *entry, int *should_skip) {
    CupError err;
    const char *entry_path;
    const char *stripped_path;
    const char *hardlink_path;
    const char *stripped_hardlink;
    char output_hardlink[MAX_PATH_LEN];
    char output_path[MAX_PATH_LEN];

    if (entry == NULL || should_skip == NULL || is_empty_string(tmp_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *should_skip = 0;

    entry_path = archive_entry_pathname(entry);
    if (entry_path == NULL || entry_path[0] == '\0') {
        fprintf(stderr, "Error: archive entry has no pathname.\n");
        return CUP_ERR_EXTRACT;
    }

    stripped_path = strip_first_path_component(entry_path);
    if (stripped_path == NULL) {
        *should_skip = 1;
        return CUP_OK;
    }

    err = path_join_safe_relative(output_path, sizeof(output_path), tmp_path, stripped_path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: archive contains an unsafe path '%s'.\n", stripped_path);
        return CUP_ERR_EXTRACT;
    }

    archive_entry_set_pathname(entry, output_path);

    hardlink_path = archive_entry_hardlink(entry);
    if (hardlink_path != NULL) {

        stripped_hardlink = strip_first_path_component(hardlink_path);
        if (stripped_hardlink == NULL) {
            fprintf(stderr, "Error: archive contains invalid hardlink target '%s'.\n", hardlink_path);
            return CUP_ERR_EXTRACT;
        }

        err = path_join_safe_relative(output_hardlink, sizeof(output_hardlink), tmp_path, stripped_hardlink);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: archive contains an unsafe hardlink target '%s'.\n", hardlink_path);
            return CUP_ERR_EXTRACT;
        }

        archive_entry_set_hardlink(entry, output_hardlink);
    }

    return CUP_OK;
}

// DATA COPY
static CupError copy_archive_data(struct archive *reader, struct archive *writer) {
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int status;

    if (reader == NULL || writer == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (;;) {
        if (interrupt_requested()) {
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_data_block(reader, &buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while reading archive data: %s.\n", archive_error_string(reader));
            return CUP_ERR_EXTRACT;
        }

        if (interrupt_requested()) {
            return CUP_ERR_INTERRUPT;
        }

        status = archive_write_data_block(writer, buffer, size, offset);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed while writing archive data: %s.\n", archive_error_string(writer));
            return CUP_ERR_EXTRACT;
        }
    }
}

// CLEANUP
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

// PUBLIC API
CupError extract_archive(const char *archive_path, const char *tmp_path) {
    CupError err;
    struct archive *reader;
    struct archive *writer;
    struct archive_entry *entry;
    int should_skip;
    int status;

    if (is_empty_string(archive_path) || is_empty_string(tmp_path)) {
        fprintf(stderr, "Error: invalid archive extraction arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_archive_open_reader(&reader, archive_path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: failed to open archive '%s'.\n", archive_path);
        return CUP_ERR_EXTRACT;
    }

    writer = archive_write_disk_new();
    if (writer == NULL) {
        fprintf(stderr, "Error: could not create libarchive writer.\n");
        cleanup_archives(reader, NULL);
        return CUP_ERR_EXTRACT;
    }

    status = archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME |
        ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: could not configure archive writer: %s.\n", archive_error_string(writer));
        cleanup_archives(reader, writer);
        return CUP_ERR_EXTRACT;
    }

    for (;;) {
        if (interrupt_requested()) {
            cleanup_archives(reader, writer);
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to read archive header: %s.\n", archive_error_string(reader));
            cleanup_archives(reader, writer);
            return CUP_ERR_EXTRACT;
        }

        if (interrupt_requested()) {
            cleanup_archives(reader, writer);
            return CUP_ERR_INTERRUPT;
        }

        err = rewrite_entry_paths(tmp_path, entry, &should_skip);
        if (err != CUP_OK) {
            cleanup_archives(reader, writer);
            return err;
        }

        if (should_skip) {
            continue;
        }

        if (interrupt_requested()) {
            cleanup_archives(reader, writer);
            return CUP_ERR_INTERRUPT;
        }

        status = archive_write_header(writer, entry);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to write archive entry '%s': %s.\n",
                archive_entry_pathname(entry), archive_error_string(writer));
            cleanup_archives(reader, writer);
            return CUP_ERR_EXTRACT;
        }

        err = copy_archive_data(reader, writer);
        if (err != CUP_OK) {
            cleanup_archives(reader, writer);
            return err;
        }

        if (interrupt_requested()) {
            cleanup_archives(reader, writer);
            return CUP_ERR_INTERRUPT;
        }

        status = archive_write_finish_entry(writer);
        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to finalize archive entry '%s': %s.\n",
                archive_entry_pathname(entry), archive_error_string(writer));
            cleanup_archives(reader, writer);
            return CUP_ERR_EXTRACT;
        }
    }

    cleanup_archives(reader, writer);
    return CUP_OK;
}