#include "extract.h"

#include "constants.h"
#include "interrupt.h"
#include "package_archive.h"
#include "path.h"
#include "text.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    char *path;
    mode_t type;
} ExtractedPath;

typedef struct {
    ExtractedPath *slots;
    size_t capacity;
    size_t count;
} ExtractedPathTable;

static uint64_t hash_path(const char *path) {
    const unsigned char *cursor = (const unsigned char *)path;
    uint64_t hash = UINT64_C(1469598103934665603);

    while (*cursor != '\0') {
        hash ^= *cursor++;
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static void path_table_free(ExtractedPathTable *table) {
    size_t i;

    if (table == NULL) {
        return;
    }

    for (i = 0; i < table->capacity; ++i) {
        free(table->slots[i].path);
    }
    free(table->slots);
    memset(table, 0, sizeof(*table));
}

static ExtractedPath *path_table_find_slot(ExtractedPathTable *table,
    const char *path) {
    size_t index = (size_t)(hash_path(path) & (table->capacity - 1));

    while (table->slots[index].path != NULL &&
        strcmp(table->slots[index].path, path) != 0) {
        index = (index + 1) & (table->capacity - 1);
    }

    return &table->slots[index];
}

static CupError path_table_resize(ExtractedPathTable *table, size_t capacity) {
    ExtractedPath *old_slots = table->slots;
    size_t old_capacity = table->capacity;
    size_t i;

    table->slots = calloc(capacity, sizeof(*table->slots));
    if (table->slots == NULL) {
        table->slots = old_slots;
        return CUP_ERR_EXTRACT;
    }

    table->capacity = capacity;
    table->count = 0;

    for (i = 0; i < old_capacity; ++i) {
        if (old_slots[i].path != NULL) {
            ExtractedPath *slot = path_table_find_slot(table, old_slots[i].path);

            *slot = old_slots[i];
            table->count++;
        }
    }

    free(old_slots);
    return CUP_OK;
}

static CupError path_table_ensure_capacity(ExtractedPathTable *table) {
    size_t capacity;

    if (table->capacity == 0) {
        return path_table_resize(table, 256);
    }
    if ((table->count + 1) * 4 < table->capacity * 3) {
        return CUP_OK;
    }

    capacity = table->capacity * 2;
    if (capacity < table->capacity) {
        return CUP_ERR_EXTRACT;
    }

    return path_table_resize(table, capacity);
}

static const ExtractedPath *path_table_find(const ExtractedPathTable *table,
    const char *path) {
    size_t index;

    if (table->capacity == 0) {
        return NULL;
    }

    index = (size_t)(hash_path(path) & (table->capacity - 1));
    while (table->slots[index].path != NULL) {
        if (strcmp(table->slots[index].path, path) == 0) {
            return &table->slots[index];
        }
        index = (index + 1) & (table->capacity - 1);
    }

    return NULL;
}

static CupError path_table_add(ExtractedPathTable *table,
    const char *path, mode_t type) {
    ExtractedPath *slot;
    size_t length;

    if (path_table_find(table, path) != NULL) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    if (path_table_ensure_capacity(table) != CUP_OK) {
        return CUP_ERR_EXTRACT;
    }

    slot = path_table_find_slot(table, path);
    length = strlen(path) + 1;
    slot->path = malloc(length);
    if (slot->path == NULL) {
        return CUP_ERR_EXTRACT;
    }

    memcpy(slot->path, path, length);
    slot->type = type;
    table->count++;
    return CUP_OK;
}

static CupError split_archive_path(const char *path, char *root,
    size_t root_size, const char **relative) {
    const char *slash;
    size_t length;

    if (text_is_empty(path) || root == NULL || root_size == 0 ||
        relative == NULL || path[0] == '/' || path[0] == '\\' ||
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

static size_t path_depth(const char *path) {
    size_t depth = 1;

    while (*path != '\0') {
        if (*path++ == '/') {
            depth++;
        }
    }

    return depth;
}

static CupError normalize_entry_path(const char *input,
    char *output, size_t size) {
    size_t length;

    if (text_is_empty(input) || output == NULL || size == 0 ||
        text_copy(output, size, input) != CUP_OK) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    length = strlen(output);
    while (length > 0 && output[length - 1] == '/') {
        output[--length] = '\0';
    }

    if (length == 0 || path_depth(output) > MAX_PACKAGE_PATH_DEPTH ||
        !path_is_safe_relative(output)) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    return CUP_OK;
}

static int symlink_target_is_internal(const char *entry_path,
    const char *target) {
    char combined[MAX_PATH_LEN];
    char copy[MAX_PATH_LEN];
    size_t count = 0;
    char *cursor;

    if (text_is_empty(entry_path) || text_is_empty(target) ||
        target[0] == '/' || target[0] == '\\' ||
        strchr(target, '\\') != NULL || strchr(target, ':') != NULL) {
        return 0;
    }

    if (text_copy(copy, sizeof(copy), entry_path) != CUP_OK) {
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
        if (text_copy(combined, sizeof(combined), target) != CUP_OK) {
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
            /* No path component is added. */
        } else if (strcmp(cursor, "..") == 0) {
            if (count == 0) {
                return 0;
            }
            count--;
        } else {
            if (!path_is_safe_segment(cursor) ||
                count >= MAX_PACKAGE_PATH_DEPTH) {
                return 0;
            }
            count++;
        }

        cursor = slash == NULL ? NULL : slash + 1;
    }

    return count > 0;
}

static CupError validate_entry_type(struct archive_entry *entry) {
    mode_t type = archive_entry_filetype(entry);
    const char *hardlink = archive_entry_hardlink(entry);

    if (hardlink != NULL) {
        archive_entry_set_filetype(entry, AE_IFREG);
        return CUP_OK;
    }

    if (type == AE_IFREG || type == AE_IFDIR || type == AE_IFLNK) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: archive contains unsupported entry type for '%s'.\n",
        archive_entry_pathname(entry));
    return CUP_ERR_ARCHIVE_UNSAFE;
}

static void normalize_entry_permissions(struct archive_entry *entry) {
    mode_t type = archive_entry_filetype(entry);
    mode_t permissions;

    if (type == AE_IFDIR) {
        permissions = 0755;
    } else if (type == AE_IFLNK) {
        permissions = 0777;
    } else {
        permissions = (archive_entry_perm(entry) & 0111) != 0 ? 0755 : 0644;
    }

    archive_entry_set_perm(entry, permissions);
    archive_entry_set_fflags(entry, 0, 0);
}

static CupError validate_entry_size(struct archive_entry *entry,
    uint64_t *declared_total) {
    la_int64_t size;

    if (archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_hardlink(entry) != NULL) {
        return CUP_OK;
    }

    size = archive_entry_size(entry);
    if (size < 0 || (uint64_t)size > MAX_PACKAGE_EXTRACTED_BYTES - *declared_total) {
        fprintf(stderr, "Error: archive exceeds the extraction size limit.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    *declared_total += (uint64_t)size;
    return CUP_OK;
}

static CupError prepare_hardlink(const char *tmp_path,
    const char *expected_root, struct archive_entry *entry,
    const ExtractedPathTable *paths) {
    const ExtractedPath *target_entry;
    const char *hardlink = archive_entry_hardlink(entry);
    const char *relative;
    char root[MAX_PATH_LEN];
    char normalized[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];

    if (hardlink == NULL) {
        return CUP_OK;
    }

    if (split_archive_path(hardlink, root, sizeof(root), &relative) != CUP_OK ||
        strcmp(root, expected_root) != 0 || relative == NULL ||
        normalize_entry_path(relative, normalized, sizeof(normalized)) != CUP_OK) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    target_entry = path_table_find(paths, normalized);
    if (target_entry == NULL || target_entry->type != AE_IFREG ||
        path_join_safe_relative(output, sizeof(output),
            tmp_path, normalized) != CUP_OK) {
        fprintf(stderr,
            "Error: archive hardlink '%s' has no previous regular-file target.\n",
            archive_entry_pathname(entry));
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    archive_entry_set_hardlink(entry, output);
    return CUP_OK;
}

static CupError prepare_archive_entry(const char *tmp_path,
    const char *expected_root, struct archive_entry *entry,
    const ExtractedPathTable *paths, char *relative_path,
    size_t relative_size, int *skip) {
    CupError err;
    const char *relative;
    const char *symlink;
    char root[MAX_PATH_LEN];
    char output[MAX_PATH_LEN];

    *skip = 0;
    relative_path[0] = '\0';

    err = split_archive_path(archive_entry_pathname(entry),
        root, sizeof(root), &relative);
    if (err != CUP_OK || strcmp(root, expected_root) != 0) {
        fprintf(stderr,
            "Error: archive contains multiple or unsafe top-level roots.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    if (relative == NULL) {
        *skip = 1;
        return CUP_OK;
    }

    err = normalize_entry_path(relative, relative_path, relative_size);
    if (err != CUP_OK || path_table_find(paths, relative_path) != NULL ||
        path_join_safe_relative(output, sizeof(output),
            tmp_path, relative_path) != CUP_OK) {
        fprintf(stderr, "Error: archive contains an unsafe or duplicate path.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    err = validate_entry_type(entry);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_hardlink(tmp_path, expected_root, entry, paths);
    if (err != CUP_OK) {
        return err;
    }

    symlink = archive_entry_symlink(entry);
    if (symlink != NULL &&
        !symlink_target_is_internal(relative_path, symlink)) {
        fprintf(stderr, "Error: archive symlink '%s' points outside the package.\n",
            relative_path);
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    normalize_entry_permissions(entry);
    archive_entry_set_pathname(entry, output);
    return CUP_OK;
}

static CupError copy_entry_data(struct archive *reader,
    struct archive *writer, la_int64_t declared_size,
    uint64_t *written_total) {
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int status;

    while (1) {
        uint64_t end;

        if (interrupt_requested()) {
            return CUP_ERR_INTERRUPT;
        }

        status = archive_read_data_block(reader, &buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return CUP_OK;
        }
        if (status != ARCHIVE_OK || offset < 0 ||
            (uint64_t)offset > UINT64_MAX - size) {
            fprintf(stderr, "Error: archive data is corrupted: %s.\n",
                archive_error_string(reader));
            return CUP_ERR_ARCHIVE;
        }

        end = (uint64_t)offset + size;
        if ((declared_size >= 0 && end > (uint64_t)declared_size) ||
            size > MAX_PACKAGE_EXTRACTED_BYTES - *written_total) {
            fprintf(stderr, "Error: archive exceeds the extraction size limit.\n");
            return CUP_ERR_ARCHIVE_UNSAFE;
        }

        if (archive_write_data_block(writer, buffer, size, offset) != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to write archive data: %s.\n",
                archive_error_string(writer));
            return CUP_ERR_EXTRACT;
        }

        *written_total += size;
    }
}

static CupError close_archives(struct archive *reader,
    struct archive *writer) {
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

CupError extract_archive(const char *archive_path, const char *tmp_path) {
    struct archive *reader = NULL;
    struct archive *writer = NULL;
    struct archive_entry *entry;
    ExtractedPathTable paths = {0};
    CupError err;
    CupError close_err;
    char root[MAX_PATH_LEN] = "";
    char relative_path[MAX_PATH_LEN];
    const char *relative;
    uint64_t declared_total = 0;
    uint64_t written_total = 0;
    size_t entry_count = 0;
    size_t extracted_count = 0;
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
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    if (status != ARCHIVE_OK) {
        fprintf(stderr, "Error: failed to configure archive extraction: %s.\n",
            archive_error_string(writer));
        close_archives(reader, writer);
        return CUP_ERR_EXTRACT;
    }

    while (1) {
        mode_t entry_type;
        la_int64_t declared_size;

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

        entry_count++;
        if (entry_count > MAX_PACKAGE_ARCHIVE_ENTRIES) {
            fprintf(stderr, "Error: archive contains too many entries.\n");
            err = CUP_ERR_ARCHIVE_UNSAFE;
            break;
        }

        if (root[0] == '\0') {
            err = split_archive_path(archive_entry_pathname(entry),
                root, sizeof(root), &relative);
            if (err != CUP_OK) {
                break;
            }
        }

        err = prepare_archive_entry(tmp_path, root, entry, &paths,
            relative_path, sizeof(relative_path), &skip);
        if (err != CUP_OK) {
            break;
        }
        if (skip) {
            continue;
        }

        err = validate_entry_size(entry, &declared_total);
        if (err != CUP_OK) {
            break;
        }

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to create archive entry '%s': %s.\n",
                archive_entry_pathname(entry), archive_error_string(writer));
            err = CUP_ERR_EXTRACT;
            break;
        }

        declared_size = archive_entry_size(entry);
        err = copy_entry_data(reader, writer, declared_size, &written_total);
        if (err != CUP_OK) {
            break;
        }

        if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
            fprintf(stderr, "Error: failed to finish archive entry '%s': %s.\n",
                archive_entry_pathname(entry), archive_error_string(writer));
            err = CUP_ERR_EXTRACT;
            break;
        }

        entry_type = archive_entry_filetype(entry);
        err = path_table_add(&paths, relative_path, entry_type);
        if (err != CUP_OK) {
            break;
        }
        extracted_count++;
    }

    path_table_free(&paths);
    close_err = close_archives(reader, writer);
    if (err == CUP_OK && close_err != CUP_OK) {
        err = close_err;
    }
    if (err == CUP_OK && extracted_count == 0) {
        err = CUP_ERR_ARCHIVE;
    }

    return err;
}
