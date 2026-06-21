#include "extract.h"

#include "constants.h"
#include "interrupt.h"
#include "package_archive.h"
#include "path.h"
#include "text.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

// ARCHIVE PATHS
static CupError split_archive_path(const char *path, char *root,
    size_t root_size, const char **relative) {
    const char *slash;
    size_t length;

    if (text_is_empty(path) || root == NULL || root_size == 0 || relative == NULL ||
        path[0] == '/' || path[0] == '\\' ||
        strchr(path, '\\') != NULL || strchr(path, ':') != NULL) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    slash = strchr(path, '/');
    if (slash == NULL) {
        length = strlen(path);

        while (length > 0 && path[length - 1] == '/') {
            length--;
        }

        if (length == 0 || length >= root_size) {
            return CUP_ERR_ARCHIVE_UNSAFE;
        }

        memcpy(root, path, length);
        root[length] = '\0';
        *relative = NULL;

        return path_is_safe_segment(root) ? CUP_OK : CUP_ERR_ARCHIVE_UNSAFE;
    }

    length = (size_t)(slash - path);
    if (length == 0 || length >= root_size) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    memcpy(root, path, length);
    root[length] = '\0';

    if (!path_is_safe_segment(root)) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    *relative = slash[1] == '\0' ? NULL : slash + 1;
    return CUP_OK;
}

static int symlink_target_is_internal(const char *entry_path, const char *target) {
    char combined[MAX_PATH_LEN];
    char copy[MAX_PATH_LEN];
    char *segments[128];
    size_t count = 0;
    char *cursor;

    if (text_is_empty(entry_path) || text_is_empty(target) ||
        target[0] == '/' || target[0] == '\\' ||
        strchr(target, '\\') != NULL || strchr(target, ':') != NULL) {
        return 0;
    }

    if (text_format(copy, sizeof(copy), "%s", entry_path) != CUP_OK) {
        return 0;
    }

    {
        char *entry_slash = strrchr(copy, '/');

        if (entry_slash != NULL) {
            *entry_slash = '\0';
        } else {
            copy[0] = '\0';
        }
    }

    if (copy[0] == '\0') {
        if (text_format(combined, sizeof(combined), "%s", target) != CUP_OK) {
            return 0;
        }
    } else if (text_format(combined, sizeof(combined),
        "%s/%s", copy, target) != CUP_OK) {
        return 0;
    }

    cursor = combined;

    while (cursor != NULL) {
        char *slash = strchr(cursor, '/');

        if (slash != NULL) {
            *slash = '\0';
        }

        if (cursor[0] == '\0' || strcmp(cursor, ".") == 0) {
            /* Ignore empty and current-directory segments. */
        } else if (strcmp(cursor, "..") == 0) {
            if (count == 0) {
                return 0;
            }
            count--;
        } else {
            if (!path_is_safe_segment(cursor) ||
                count >= sizeof(segments) / sizeof(segments[0])) {
                return 0;
            }

            segments[count++] = cursor;
        }

        cursor = slash == NULL ? NULL : slash + 1;
    }

    return count > 0;
}

static CupError normalize_entry_path(const char *input, char *output, size_t size) {
    size_t length;

    if (text_is_empty(input) || output == NULL || size == 0) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    if (text_format(output, size, "%s", input) != CUP_OK) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    length = strlen(output);
    while (length > 0 && output[length - 1] == '/') {
        output[--length] = '\0';
    }

    if (length == 0 || !path_is_safe_relative(output)) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    return CUP_OK;
}

// ARCHIVE ENTRIES
static CupError validate_entry_type(struct archive_entry *entry) {
    mode_t type;

    type = archive_entry_filetype(entry);

    if (archive_entry_hardlink(entry) != NULL ||
        type == AE_IFREG || type == AE_IFDIR || type == AE_IFLNK) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: archive contains unsupported entry type for '%s'.\n",
        archive_entry_pathname(entry));
    return CUP_ERR_ARCHIVE_UNSAFE;
}

static CupError prepare_archive_entry(const char *tmp_path, const char *expected_root,
    struct archive_entry *entry, int *skip) {
    CupError err;
    char root[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];
    char relative_path[MAX_PATH_LEN];
    char hardlink_root[MAX_PATH_LEN];
    char hardlink_output[MAX_PATH_LEN];
    const char *relative;
    const char *hardlink;
    const char *hardlink_relative;
    const char *symlink;

    *skip = 0;

    err = split_archive_path(archive_entry_pathname(entry),
        root, sizeof(root), &relative);
    if (err != CUP_OK || strcmp(root, expected_root) != 0) {
        fprintf(stderr, "Error: archive contains multiple or unsafe top-level roots.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    if (relative == NULL) {
        *skip = 1;
        return CUP_OK;
    }

    if (normalize_entry_path(relative, relative_path, sizeof(relative_path)) != CUP_OK ||
        path_join_safe_relative(output, sizeof(output),
            tmp_path, relative_path) != CUP_OK) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    err = validate_entry_type(entry);
    if (err != CUP_OK) {
        return err;
    }

    hardlink = archive_entry_hardlink(entry);
    if (hardlink != NULL) {
        err = split_archive_path(hardlink, hardlink_root,
            sizeof(hardlink_root), &hardlink_relative);
        if (err != CUP_OK || strcmp(hardlink_root, expected_root) != 0 ||
            hardlink_relative == NULL || !path_is_safe_relative(hardlink_relative) ||
            path_join_safe_relative(hardlink_output, sizeof(hardlink_output),
                tmp_path, hardlink_relative) != CUP_OK) {
            return CUP_ERR_ARCHIVE_UNSAFE;
        }

        archive_entry_set_hardlink(entry, hardlink_output);
    }

    symlink = archive_entry_symlink(entry);
    if (symlink != NULL && !symlink_target_is_internal(relative_path, symlink)) {
        fprintf(stderr, "Error: archive symlink '%s' points outside the package.\n",
            relative_path);
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    archive_entry_set_pathname(entry, output);
    return CUP_OK;
}

static CupError copy_entry_data(struct archive *reader, struct archive *writer) {
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int status;

    while (1) {
        if (interrupt_requested()) {
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_data_block(reader, &buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }

        if (status != ARCHIVE_OK) {
            fprintf(stderr, "Error: archive data is corrupted: %s.\n",
                archive_error_string(reader));
            return CUP_ERR_ARCHIVE;
        }

        if (archive_write_data_block(writer, buffer, size, offset) != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to write archive data: %s.\n",
                archive_error_string(writer));
            return CUP_ERR_EXTRACT;
        }
    }
}

static CupError close_archives(struct archive *reader, struct archive *writer) {
    int failed = 0;

    if (reader != NULL) {
        if (archive_read_close(reader) != ARCHIVE_OK) {
            failed = 1;
        }
        if (archive_read_free(reader) != ARCHIVE_OK) {
            failed = 1;
        }
    }

    if (writer != NULL) {
        if (archive_write_close(writer) != ARCHIVE_OK) {
            failed = 1;
        }
        if (archive_write_free(writer) != ARCHIVE_OK) {
            failed = 1;
        }
    }

    return failed ? CUP_ERR_EXTRACT : CUP_OK;
}

// EXTRACTION
CupError extract_archive(const char *archive_path, const char *tmp_path) {
    struct archive *reader = NULL;
    struct archive *writer = NULL;
    struct archive_entry *entry;
    CupError err;
    CupError close_err;
    char root[MAX_PATH_LEN] = "";
    const char *relative;
    size_t extracted = 0;
    int status;
    int skip;

    if (text_is_empty(archive_path) || text_is_empty(tmp_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_archive_open_reader(&reader, archive_path);
    if (err != CUP_OK) {
        return CUP_ERR_ARCHIVE;
    }

    writer = archive_write_disk_new();
    if (writer == NULL) {
        close_archives(reader, NULL);
        return CUP_ERR_EXTRACT;
    }

    status = archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    if (status != ARCHIVE_OK) {
        close_archives(reader, writer);
        return CUP_ERR_EXTRACT;
    }

    while (1) {
        if (interrupt_requested()) {
            err = CUP_ERR_INTERRUPT;
            break;
        }

        status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF) {
            err = CUP_OK;
            break;
        }

        if (status != ARCHIVE_OK) {
            err = CUP_ERR_ARCHIVE;
            break;
        }

        if (root[0] == '\0') {
            err = split_archive_path(archive_entry_pathname(entry),
                root, sizeof(root), &relative);
            if (err != CUP_OK) {
                break;
            }
        }

        err = prepare_archive_entry(tmp_path, root, entry, &skip);
        if (err != CUP_OK) {
            break;
        }

        if (skip) {
            continue;
        }

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            err = CUP_ERR_EXTRACT;
            break;
        }

        err = copy_entry_data(reader, writer);
        if (err != CUP_OK) {
            break;
        }

        if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
            err = CUP_ERR_EXTRACT;
            break;
        }

        extracted++;
    }

    close_err = close_archives(reader, writer);
    if (err == CUP_OK && close_err != CUP_OK) {
        err = close_err;
    }

    if (err == CUP_OK && extracted == 0) {
        err = CUP_ERR_ARCHIVE;
    }

    return err;
}
