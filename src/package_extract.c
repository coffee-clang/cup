/*
 * Extracts one bounded package archive below an anchored staging directory. Archive paths use a
 * portable ASCII grammar, case-fold collisions and file/directory aliases are rejected, only
 * directories and regular files are admitted, and permissions are normalized rather than trusted
 * from the archive.
 */

#include "package_extract.h"

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

#define HASH_NONFATAL_OOM 1
#define uthash_nonfatal_oom(element) ((element)->hash_oom = 1)
#include "uthash.h"

#if defined(_WIN32)
#include "windows_utf.h"

#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef enum {
    PATH_STATE_ANCESTOR_ONLY,
    PATH_STATE_EXPLICIT_DIRECTORY,
    PATH_STATE_EXPLICIT_FILE
} ExtractedPathState;

typedef struct ExtractedPath {
    ExtractedPathState state;
    int hash_oom;
    UT_hash_handle hh;
    char key[];
} ExtractedPath;

typedef struct {
    ExtractedPath *paths;
    uint64_t allocated_bytes;
} ExtractedPathTable;

typedef struct {
#if defined(_WIN32)
    wchar_t previous[MAX_PATH_LEN];
#else
    int previous_fd;
    int root_fd;
#endif
    int active;
} ExtractionRoot;

/* Portable path state. One case-folded table represents both explicit entries and prefixes that
 * already own descendants, so file/directory alias policy has one mutation owner. */
static CupError lowercase_path(const char *path, char *key, size_t size) {
    size_t length;
    size_t i;

    if (text_is_empty(path) || key == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    length = strlen(path);
    if (length >= size) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }
    for (i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)path[i];
        key[i] = value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : (char)value;
    }
    key[length] = '\0';
    return CUP_OK;
}

static void path_table_free(ExtractedPathTable *table) {
    ExtractedPath *entry;
    ExtractedPath *next;

    if (table == NULL) {
        return;
    }
    entry = table->paths;
    HASH_CLEAR(hh, table->paths);
    while (entry != NULL) {
        next = (ExtractedPath *)entry->hh.next;
        free(entry);
        entry = next;
    }
    table->allocated_bytes = 0;
}

static ExtractedPath *path_table_find(const ExtractedPathTable *table, const char *key) {
    ExtractedPath *entry = NULL;

    if (table == NULL || key == NULL) {
        return NULL;
    }
    HASH_FIND_STR(table->paths, key, entry);
    return entry;
}

static CupError path_table_add(ExtractedPathTable *table,
                               const char *key,
                               ExtractedPathState state) {
    ExtractedPath *entry;
    size_t length;
    uint64_t allocation_size;

    length = strlen(key) + 1;
    allocation_size = (uint64_t)sizeof(*entry) + length;
    if (allocation_size > MAX_PACKAGE_PATH_TABLE_BYTES - table->allocated_bytes) {
        fprintf(stderr, "Error: archive path state exceeds the resource limit.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }
    entry = calloc(1, (size_t)allocation_size);
    if (entry == NULL) {
        fprintf(stderr, "Error: failed to allocate archive path state.\n");
        return CUP_ERR_EXTRACT;
    }
    entry->state = state;
    memcpy(entry->key, key, length);
    HASH_ADD_KEYPTR(hh, table->paths, entry->key, strlen(entry->key), entry);
    if (entry->hash_oom) {
        free(entry);
        fprintf(stderr, "Error: failed to allocate archive path state.\n");
        return CUP_ERR_EXTRACT;
    }
    table->allocated_bytes += allocation_size;
    return CUP_OK;
}

/* Register one path before any disk write. Mutation failure is terminal for the extraction attempt,
 * so partial in-memory registration needs no rollback and the whole table is freed on exit. */
static CupError path_table_register(ExtractedPathTable *table, const char *path, mode_t type) {
    char key[MAX_PATH_LEN];
    char *slash;
    ExtractedPath *entry;
    ExtractedPathState explicit_state;
    CupError err;

    if (table == NULL || (type != AE_IFDIR && type != AE_IFREG)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = lowercase_path(path, key, sizeof(key));
    if (err != CUP_OK) {
        return err;
    }

    entry = path_table_find(table, key);
    if (entry != NULL &&
        !(entry->state == PATH_STATE_ANCESTOR_ONLY && type == AE_IFDIR)) {
        fprintf(stderr,
                "Error: archive contains a duplicate, case-colliding, or "
                "file/directory-colliding path.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    slash = key;
    while ((slash = strchr(slash, '/')) != NULL) {
        char saved = *slash;
        *slash = '\0';
        entry = path_table_find(table, key);
        *slash = saved;
        if (entry != NULL && entry->state == PATH_STATE_EXPLICIT_FILE) {
            fprintf(stderr,
                    "Error: archive contains a duplicate, case-colliding, or "
                    "file/directory-colliding path.\n");
            return CUP_ERR_ARCHIVE_UNSAFE;
        }
        slash++;
    }

    slash = key;
    while ((slash = strchr(slash, '/')) != NULL) {
        char saved = *slash;
        *slash = '\0';
        if (path_table_find(table, key) == NULL) {
            err = path_table_add(table, key, PATH_STATE_ANCESTOR_ONLY);
            if (err != CUP_OK) {
                *slash = saved;
                return err;
            }
        }
        *slash = saved;
        slash++;
    }

    entry = path_table_find(table, key);
    if (entry != NULL) {
        entry->state = PATH_STATE_EXPLICIT_DIRECTORY;
        return CUP_OK;
    }

    explicit_state = type == AE_IFDIR ? PATH_STATE_EXPLICIT_DIRECTORY : PATH_STATE_EXPLICIT_FILE;
    return path_table_add(table, key, explicit_state);
}

/* Entry-path validation. Archive names are normalized before any destination path is
 * constructed. */
static CupError split_archive_path(const char *path,
                                   char *root,
                                   size_t root_size,
                                   const char **relative) {
    const char *slash;
    size_t length;

    if (text_is_empty(path) || root == NULL || root_size == 0 || relative == NULL ||
        path[0] == '/' || path[0] == '\\' || strchr(path, '\\') != NULL ||
        strchr(path, ':') != NULL) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    slash = strchr(path, '/');
    length = slash == NULL ? strlen(path) : (size_t)(slash - path);
    if (length == 0 || length >= root_size) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    memcpy(root, path, length);
    root[length] = '\0';
    if (!path_is_safe_segment(root)) {
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    *relative = slash == NULL || slash[1] == '\0' ? NULL : slash + 1;
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

static CupError normalize_entry_path(const char *input, char *output, size_t size) {
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

/* Entry policy and accounting. Only directories and regular files with bounded sizes are
 * admitted. */
static CupError validate_entry_type(struct archive_entry *entry) {
    mode_t type = archive_entry_filetype(entry);

    if (archive_entry_hardlink(entry) == NULL && archive_entry_symlink(entry) == NULL &&
        (type == AE_IFREG || type == AE_IFDIR)) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: archive contains unsupported entry type for '%s'.\n",
            archive_entry_pathname(entry));
    return CUP_ERR_ARCHIVE_UNSAFE;
}

static void normalize_entry_metadata(struct archive_entry *entry) {
    mode_t type = archive_entry_filetype(entry);
    mode_t permissions;

    if (type == AE_IFDIR) {
        permissions = 0755;
    } else {
        permissions = (archive_entry_perm(entry) & 0111) != 0 ? 0755 : 0644;
    }

    archive_entry_set_perm(entry, permissions);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_uname(entry, NULL);
    archive_entry_set_gname(entry, NULL);
    archive_entry_set_fflags(entry, 0, 0);
    archive_entry_unset_mtime(entry);
    archive_entry_unset_atime(entry);
    archive_entry_unset_ctime(entry);
}

static CupError validate_entry_size(struct archive_entry *entry, uint64_t *declared_total) {
    la_int64_t size;

    if (archive_entry_filetype(entry) != AE_IFREG) {
        return CUP_OK;
    }

    size = archive_entry_size(entry);
    if (size < 0 || (uint64_t)size > MAX_PACKAGE_ENTRY_BYTES ||
        (uint64_t)size > MAX_PACKAGE_EXTRACTED_BYTES - *declared_total) {
        fprintf(stderr, "Error: archive exceeds the extraction size limit.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    *declared_total += (uint64_t)size;
    return CUP_OK;
}

static CupError prepare_archive_entry(const char *expected_root,
                                      struct archive_entry *entry,
                                      char *relative_path,
                                      size_t relative_size,
                                      int *root_entry_seen,
                                      int *skip) {
    CupError err;
    const char *relative;
    char root[MAX_PATH_LEN];

    if (root_entry_seen == NULL || skip == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *skip = 0;
    relative_path[0] = '\0';

    err = split_archive_path(archive_entry_pathname(entry), root, sizeof(root), &relative);
    if (err != CUP_OK || strcmp(root, expected_root) != 0) {
        fprintf(stderr, "Error: archive contains multiple or unsafe top-level roots.\n");
        return CUP_ERR_ARCHIVE_UNSAFE;
    }

    if (relative == NULL) {
        if (*root_entry_seen) {
            fprintf(stderr, "Error: archive repeats its explicit top-level root.\n");
            return CUP_ERR_ARCHIVE_UNSAFE;
        }
        if (archive_entry_filetype(entry) != AE_IFDIR || archive_entry_hardlink(entry) != NULL ||
            archive_entry_symlink(entry) != NULL) {
            fprintf(stderr, "Error: archive top-level root is not a directory.\n");
            return CUP_ERR_ARCHIVE_UNSAFE;
        }
        *root_entry_seen = 1;
        *skip = 1;
        return CUP_OK;
    }

    err = normalize_entry_path(relative, relative_path, relative_size);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: archive contains an unsafe path.\n");
        return err;
    }

    err = validate_entry_type(entry);
    if (err != CUP_OK) {
        return err;
    }

    normalize_entry_metadata(entry);
    archive_entry_set_pathname(entry, relative_path);
    return CUP_OK;
}

static CupError copy_entry_data(struct archive *reader,
                                struct archive *writer,
                                la_int64_t declared_size,
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
        if (status != ARCHIVE_OK || offset < 0 || (uint64_t)offset > UINT64_MAX - size) {
            fprintf(
                stderr, "Error: archive data is corrupted: %s.\n", archive_error_string(reader));
            return CUP_ERR_ARCHIVE;
        }

        end = (uint64_t)offset + size;
        if ((declared_size >= 0 && end > (uint64_t)declared_size) ||
            size > MAX_PACKAGE_EXTRACTED_BYTES - *written_total) {
            fprintf(stderr, "Error: archive exceeds the extraction size limit.\n");
            return CUP_ERR_ARCHIVE_UNSAFE;
        }

        if (archive_write_data_block(writer, buffer, size, offset) != ARCHIVE_OK) {
            fprintf(
                stderr, "Error: failed to write archive data: %s.\n", archive_error_string(writer));
            return CUP_ERR_EXTRACT;
        }

        *written_total += size;
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

#if defined(_WIN32)
/* Anchored extraction root. Platform-specific setup prevents archive entries from redirecting later
 * writes outside staging. */
static CupError enter_extraction_root(const char *path, ExtractionRoot *root) {
    wchar_t wide_path[MAX_PATH_LEN];
    DWORD previous_length;

    previous_length = GetCurrentDirectoryW(MAX_PATH_LEN, root->previous);
    if (previous_length == 0 || previous_length >= MAX_PATH_LEN ||
        windows_utf8_to_wide_path(path, wide_path, MAX_PATH_LEN) != CUP_OK ||
        !SetCurrentDirectoryW(wide_path)) {
        return CUP_ERR_FILESYSTEM;
    }
    root->active = 1;
    return CUP_OK;
}

static CupError leave_extraction_root(ExtractionRoot *root) {
    if (!root->active) {
        return CUP_OK;
    }
    root->active = 0;
    return SetCurrentDirectoryW(root->previous) ? CUP_OK : CUP_ERR_FILESYSTEM;
}
#else
static CupError enter_extraction_root(const char *path, ExtractionRoot *root) {
    struct stat status;

    root->previous_fd = -1;
    root->root_fd = -1;
    root->active = 0;

    root->previous_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root->previous_fd < 0) {
        return CUP_ERR_FILESYSTEM;
    }
    root->root_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root->root_fd < 0 || fstat(root->root_fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
        fchdir(root->root_fd) != 0) {
        if (root->root_fd >= 0) {
            close(root->root_fd);
        }
        close(root->previous_fd);
        root->root_fd = -1;
        root->previous_fd = -1;
        return CUP_ERR_FILESYSTEM;
    }

    root->active = 1;
    return CUP_OK;
}

static CupError leave_extraction_root(ExtractionRoot *root) {
    int failed = 0;

    if (!root->active) {
        return CUP_OK;
    }
    if (fchdir(root->previous_fd) != 0) {
        failed = 1;
    }
    if (close(root->root_fd) != 0) {
        failed = 1;
    }
    if (close(root->previous_fd) != 0) {
        failed = 1;
    }
    root->active = 0;
    return failed ? CUP_ERR_FILESYSTEM : CUP_OK;
}
#endif

/* Extraction transaction. Every entry is validated before it is written. The caller owns the
 * staging transaction and removes partial output when this function reports a failure. */
static CupError package_extract_reader(struct archive *reader,
                                       const char *staging_path,
                                       PackageArchiveFormat expected_format,
                                       const char *format_value) {
    struct archive *writer = NULL;
    struct archive_entry *entry;
    ExtractedPathTable paths = {0};
    ExtractionRoot extraction_root = {0};
    CupError err;
    CupError close_err;
    CupError leave_err;
    char root[MAX_PATH_LEN] = "";
    char relative_path[MAX_PATH_LEN];
    const char *relative;
    uint64_t declared_total = 0;
    uint64_t written_total = 0;
    size_t entry_count = 0;
    size_t extracted_count = 0;
    int format_checked = 0;
    int root_entry_seen = 0;
    int status;
    int skip;

    if (reader == NULL || text_is_empty(staging_path) || text_is_empty(format_value)) {
        if (reader != NULL) {
            close_archives(reader, NULL);
        }
        return CUP_ERR_INVALID_INPUT;
    }

    err = enter_extraction_root(staging_path, &extraction_root);
    if (err != CUP_OK) {
        close_archives(reader, NULL);
        return err;
    }

    /* Anchor extraction in the caller's fresh staging directory and configure safe writes. */
    writer = archive_write_disk_new();
    if (writer == NULL) {
        leave_extraction_root(&extraction_root);
        close_archives(reader, NULL);
        return CUP_ERR_EXTRACT;
    }

    status = archive_write_disk_set_options(writer,
                                            ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS |
                                                ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                                ARCHIVE_EXTRACT_NO_OVERWRITE);
    if (status != ARCHIVE_OK) {
        fprintf(stderr,
                "Error: failed to configure archive extraction: %s.\n",
                archive_error_string(writer));
        leave_extraction_root(&extraction_root);
        close_archives(reader, writer);
        return CUP_ERR_EXTRACT;
    }

    /* Validate and register one entry before writing it and advancing to the next header. */
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

        if (!format_checked) {
            format_checked = 1;
            if (!package_archive_reader_matches_format(reader, expected_format)) {
                fprintf(stderr,
                        "Error: archive content does not match declared format '%s'.\n",
                        format_value);
                err = CUP_ERR_ARCHIVE_UNSAFE;
                break;
            }
        }

        entry_count++;
        if (entry_count > MAX_PACKAGE_ARCHIVE_ENTRIES) {
            fprintf(stderr, "Error: archive contains too many entries.\n");
            err = CUP_ERR_ARCHIVE_UNSAFE;
            break;
        }

        if (root[0] == '\0') {
            err = split_archive_path(archive_entry_pathname(entry), root, sizeof(root), &relative);
            if (err != CUP_OK) {
                break;
            }
        }

        err =
            prepare_archive_entry(root,
                                  entry,
                                  relative_path,
                                  sizeof(relative_path),
                                  &root_entry_seen,
                                  &skip);
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

        entry_type = archive_entry_filetype(entry);
        err = path_table_register(&paths, relative_path, entry_type);
        if (err != CUP_OK) {
            break;
        }

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            fprintf(stderr,
                    "Error: failed to create archive entry '%s': %s.\n",
                    archive_entry_pathname(entry),
                    archive_error_string(writer));
            err = CUP_ERR_EXTRACT;
            break;
        }

        declared_size = archive_entry_size(entry);
        err = copy_entry_data(reader, writer, declared_size, &written_total);
        if (err != CUP_OK) {
            break;
        }

        if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
            fprintf(stderr,
                    "Error: failed to finish archive entry '%s': %s.\n",
                    archive_entry_pathname(entry),
                    archive_error_string(writer));
            err = CUP_ERR_EXTRACT;
            break;
        }

        extracted_count++;
    }

    /* Close all resources before deciding whether a complete payload was produced. */
    path_table_free(&paths);
    close_err = close_archives(reader, writer);
    leave_err = leave_extraction_root(&extraction_root);
    if (err == CUP_OK && close_err != CUP_OK) {
        err = close_err;
    }
    if (err == CUP_OK && leave_err != CUP_OK) {
        err = leave_err;
    }
    if (err == CUP_OK && (!format_checked || extracted_count == 0)) {
        err = CUP_ERR_ARCHIVE;
    }

    return err;
}

CupError package_extract_verified(VerifiedArtifact *artifact, const char *staging_path) {
    struct archive *reader = NULL;
    const char *format_name;
    CupError err;

    if (artifact == NULL || artifact->file == NULL || text_is_empty(staging_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    format_name = package_archive_format_name(artifact->format);
    if (format_name == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = package_archive_open_stream(&reader, artifact->file);
    if (err != CUP_OK) {
        return CUP_ERR_ARCHIVE;
    }
    return package_extract_reader(reader, staging_path, artifact->format, format_name);
}
