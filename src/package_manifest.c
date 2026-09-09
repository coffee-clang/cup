/*
 * Verifies cup-components manifest.txt format=2 against the complete extracted package tree.
 */

#include "package_manifest.h"

#include "checksum.h"
#include "constants.h"
#include "interrupt.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#define PACKAGE_MANIFEST_LINE_LEN (MAX_PATH_LEN + 80u)

/* Folded paths retain the producer's cross-platform no-case-collision property. */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
    size_t bytes;
} ManifestPathKeys;

typedef struct {
    const char *manifest_path;
    size_t count;
    unsigned depth;
} TreeCountContext;

static void manifest_diagnostic(FILE *diagnostics, const char *message, size_t line_number) {
    if (diagnostics == NULL) {
        return;
    }
    if (line_number == 0) {
        fprintf(diagnostics, "Error: %s.\n", message);
    } else {
        fprintf(diagnostics, "Error: %s on package manifest line %zu.\n", message, line_number);
    }
}

static CupError read_manifest_line(FILE *file,
                                   char *line,
                                   size_t size,
                                   size_t *line_number,
                                   int *has_line) {
    size_t length;
    size_t i;

    if (file == NULL || line == NULL || size < 2 || line_number == NULL || has_line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_line = 0;
    if (fgets(line, (int)size, file) == NULL) {
        return feof(file) ? CUP_OK : CUP_ERR_FILESYSTEM;
    }

    length = strlen(line);
    if (length == 0 || line[length - 1] != '\n') {
        return CUP_ERR_VALIDATION;
    }
    line[--length] = '\0';
    if (length == 0) {
        return CUP_ERR_VALIDATION;
    }
    for (i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)line[i];

        if (value != '\t' && (value < 0x20u || value > 0x7eu)) {
            return CUP_ERR_VALIDATION;
        }
    }

    (*line_number)++;
    *has_line = 1;
    return CUP_OK;
}

static CupError split_manifest_entry(char *line,
                                     char **type,
                                     char **mode,
                                     char **digest,
                                     char **relative) {
    char *first;
    char *second;
    char *third;

    if (line == NULL || type == NULL || mode == NULL || digest == NULL || relative == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    first = strchr(line, '\t');
    if (first == NULL) {
        return CUP_ERR_VALIDATION;
    }
    *first++ = '\0';
    second = strchr(first, '\t');
    if (second == NULL) {
        return CUP_ERR_VALIDATION;
    }
    *second++ = '\0';
    third = strchr(second, '\t');
    if (third == NULL || strchr(third + 1, '\t') != NULL) {
        return CUP_ERR_VALIDATION;
    }
    *third++ = '\0';

    if (line[0] == '\0' || first[0] == '\0' || second[0] == '\0' || third[0] == '\0') {
        return CUP_ERR_VALIDATION;
    }
    *type = line;
    *mode = first;
    *digest = second;
    *relative = third;
    return CUP_OK;
}

#if defined(_WIN32)
static int windows_mode_extension_is_executable(const char *path) {
    const char *extension = strrchr(path, '.');
    static const char *const extensions[] = {".exe", ".com", ".bat", ".cmd", ".dll", ".pyd"};
    size_t i;

    if (extension == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        if (text_equal_ascii_ignore_case(extension, extensions[i])) {
            return 1;
        }
    }
    return 0;
}

static CupError regular_file_mode(FILE *file, const char *path, unsigned *mode) {
    unsigned char prefix[2] = {0, 0};
    size_t count;

    if (file == NULL || text_is_empty(path) || mode == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (windows_mode_extension_is_executable(path)) {
        *mode = 0755u;
        return CUP_OK;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    count = fread(prefix, 1, sizeof(prefix), file);
    if (ferror(file)) {
        return CUP_ERR_FILESYSTEM;
    }
    *mode = count == 2 && prefix[0] == '#' && prefix[1] == '!' ? 0755u : 0644u;
    return fseek(file, 0, SEEK_SET) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}
#else
static CupError path_mode(const char *path, unsigned *mode) {
    struct stat status;

    if (text_is_empty(path) || mode == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (lstat(path, &status) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *mode = (unsigned)(status.st_mode & 0777u);
    return CUP_OK;
}
#endif

static CupError verify_regular_file(const char *path,
                                    const char *expected_mode,
                                    const char *expected_digest) {
    SystemPathIdentity identity;
    FILE *file = NULL;
    uint64_t file_size = 0;
    char digest[65];
    unsigned mode;
    int missing = 0;
    CupError err;

    memset(&identity, 0, sizeof(identity));
    err = system_open_regular_file(path, &file, &identity, &file_size, &missing);
    (void)file_size;
    if (err != CUP_OK || missing || file == NULL) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }

#if defined(_WIN32)
    err = regular_file_mode(file, path, &mode);
#else
    err = path_mode(path, &mode);
#endif
    if (err == CUP_OK) {
        unsigned expected = strcmp(expected_mode, "0755") == 0 ? 0755u : 0644u;
        if (mode != expected) {
            err = CUP_ERR_VALIDATION;
        }
    }
    if (err == CUP_OK && fseek(file, 0, SEEK_SET) != 0) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err == CUP_OK) {
        err = checksum_sha256_stream(file, digest, sizeof(digest));
    }
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err == CUP_OK && strcmp(digest, expected_digest) != 0) {
        err = CUP_ERR_VALIDATION;
    }
    return err;
}

static CupError verify_directory(const char *path, const char *expected_mode) {
    SystemPathKind kind;
    CupError err = system_get_path_kind(path, &kind);

    if (err != CUP_OK) {
        return err;
    }
    if (kind != SYSTEM_PATH_DIRECTORY || strcmp(expected_mode, "0755") != 0) {
        return CUP_ERR_VALIDATION;
    }
#if !defined(_WIN32)
    {
        unsigned mode;
        err = path_mode(path, &mode);
        if (err != CUP_OK) {
            return err;
        }
        if (mode != 0755u) {
            return CUP_ERR_VALIDATION;
        }
    }
#endif
    return CUP_OK;
}

static CupError verify_link(const char *base_path,
                            const char *relative,
                            const char *path,
                            const char *expected_digest) {
#if defined(_WIN32)
    (void)base_path;
    (void)relative;
    (void)path;
    (void)expected_digest;
    return CUP_ERR_VALIDATION;
#else
    char target[MAX_PATH_LEN];
    char digest[65];
    ssize_t length;
    FILE *resolved = NULL;
    SystemPathIdentity identity;
    uint64_t size = 0;
    int missing = 0;
    CupError err;

    length = readlink(path, target, sizeof(target));
    if (length <= 0 || (size_t)length >= sizeof(target)) {
        return CUP_ERR_VALIDATION;
    }
    target[length] = '\0';
    if (strchr(target, '\n') != NULL || strchr(target, '\r') != NULL) {
        return CUP_ERR_VALIDATION;
    }
    err = checksum_sha256_bytes((const unsigned char *)target, (size_t)length, digest, sizeof(digest));
    if (err != CUP_OK || strcmp(digest, expected_digest) != 0) {
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }

    memset(&identity, 0, sizeof(identity));
    err = system_open_regular_file_beneath(
        base_path, relative, &resolved, &identity, &size, &missing);
    (void)size;
    if (resolved != NULL && fclose(resolved) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err != CUP_OK || missing) {
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }
    return CUP_OK;
#endif
}

static CupError verify_manifest_entry(const char *base_path,
                                      const char *host_platform,
                                      const char *type,
                                      const char *mode,
                                      const char *digest,
                                      const char *relative) {
    char path[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    if (!path_is_safe_relative(relative) || strcmp(relative, CUP_MANIFEST_FILENAME) == 0 ||
        strcmp(relative, ".manifest.paths") == 0 ||
        path_join_safe_relative(path, sizeof(path), base_path, relative) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }

    if (strcmp(type, "f") == 0) {
        if (kind != SYSTEM_PATH_REGULAR_FILE ||
            (strcmp(mode, "0644") != 0 && strcmp(mode, "0755") != 0) ||
            !checksum_digest_is_canonical(digest)) {
            return CUP_ERR_VALIDATION;
        }
        return verify_regular_file(path, mode, digest);
    }
    if (strcmp(type, "d") == 0) {
        if (kind != SYSTEM_PATH_DIRECTORY || strcmp(digest, "-") != 0) {
            return CUP_ERR_VALIDATION;
        }
        return verify_directory(path, mode);
    }
    if (strcmp(type, "l") == 0) {
        if (strcmp(host_platform, "windows-x64") == 0 || kind != SYSTEM_PATH_LINK ||
            strcmp(mode, "-") != 0 || !checksum_digest_is_canonical(digest)) {
            return CUP_ERR_VALIDATION;
        }
        return verify_link(base_path, relative, path, digest);
    }
    return CUP_ERR_VALIDATION;
}

static CupError count_tree_entry(const char *path,
                                 SystemPathKind kind,
                                 const SystemPathIdentity *identity,
                                 void *userdata) {
    TreeCountContext *context = userdata;
    TreeCountContext child;
    CupError err;

    if (context == NULL || identity == NULL || !identity->valid || identity->kind != kind) {
        return CUP_ERR_FILESYSTEM;
    }
    if (path_equal(path, context->manifest_path)) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE && kind != SYSTEM_PATH_DIRECTORY &&
        kind != SYSTEM_PATH_LINK) {
        return CUP_ERR_VALIDATION;
    }
#if defined(_WIN32)
    if (kind == SYSTEM_PATH_LINK) {
        return CUP_ERR_VALIDATION;
    }
#endif
    context->count++;
    if (kind != SYSTEM_PATH_DIRECTORY) {
        return interrupt_requested() ? CUP_ERR_INTERRUPT : CUP_OK;
    }
    if (context->depth >= MAX_PACKAGE_PATH_DEPTH) {
        return CUP_ERR_VALIDATION;
    }

    child = *context;
    child.depth++;
    err = system_list_directory(path, count_tree_entry, &child);
    if (err == CUP_OK) {
        context->count = child.count;
    }
    return err;
}

static CupError count_package_tree(const char *base_path,
                                   const char *manifest_path,
                                   size_t *count) {
    TreeCountContext context;
    CupError err;

    if (text_is_empty(base_path) || text_is_empty(manifest_path) || count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    context.manifest_path = manifest_path;
    context.count = 0;
    context.depth = 0;
    err = system_list_directory(base_path, count_tree_entry, &context);
    if (err == CUP_OK) {
        *count = context.count;
    }
    return err;
}

static int compare_path_keys(const void *left, const void *right) {
    const char *const *left_key = left;
    const char *const *right_key = right;

    return strcmp(*left_key, *right_key);
}

static void free_path_keys(ManifestPathKeys *keys) {
    size_t i;

    if (keys == NULL) {
        return;
    }
    for (i = 0; i < keys->count; ++i) {
        free(keys->items[i]);
    }
    free(keys->items);
    memset(keys, 0, sizeof(*keys));
}

static CupError add_folded_path(ManifestPathKeys *keys, const char *relative) {
    char folded[MAX_PATH_LEN];
    char *copy;
    char **items;
    size_t length;
    size_t capacity = keys != NULL ? keys->capacity : 0;
    size_t allocation_bytes = 0;
    size_t string_bytes;

    if (keys == NULL || text_is_empty(relative) ||
        text_copy_lower_ascii(folded, sizeof(folded), relative) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    length = strlen(folded);
    string_bytes = length + 1u;
    if (keys->count == keys->capacity) {
        capacity = keys->capacity == 0 ? 64u : keys->capacity * 2u;
        if (capacity < keys->capacity || capacity > MAX_PACKAGE_ARCHIVE_ENTRIES ||
            capacity > SIZE_MAX / sizeof(*items)) {
            return CUP_ERR_VALIDATION;
        }
        allocation_bytes = (capacity - keys->capacity) * sizeof(*items);
    }
    if (allocation_bytes > MAX_PACKAGE_PATH_TABLE_BYTES - keys->bytes ||
        string_bytes > MAX_PACKAGE_PATH_TABLE_BYTES - keys->bytes - allocation_bytes) {
        return CUP_ERR_VALIDATION;
    }

    if (allocation_bytes != 0) {
        items = realloc(keys->items, capacity * sizeof(*items));
        if (items == NULL) {
            return CUP_ERR_TEMPORARY;
        }
        keys->items = items;
        keys->bytes += allocation_bytes;
        keys->capacity = capacity;
    }

    copy = malloc(string_bytes);
    if (copy == NULL) {
        return CUP_ERR_TEMPORARY;
    }
    memcpy(copy, folded, string_bytes);
    keys->items[keys->count++] = copy;
    keys->bytes += string_bytes;
    return CUP_OK;
}

static int path_keys_have_collision(ManifestPathKeys *keys) {
    size_t i;

    if (keys == NULL || keys->count < 2) {
        return 0;
    }
    qsort(keys->items, keys->count, sizeof(keys->items[0]), compare_path_keys);
    for (i = 1; i < keys->count; ++i) {
        if (strcmp(keys->items[i - 1], keys->items[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

CupError package_manifest_verify(const char *base_path,
                                 const char *host_platform,
                                 FILE *diagnostics) {
    ManifestPathKeys path_keys = {0};
    SystemPathIdentity identity;
    FILE *file = NULL;
    uint64_t file_size = 0;
    char manifest_path[MAX_PATH_LEN];
    char line[PACKAGE_MANIFEST_LINE_LEN];
    char previous[MAX_PATH_LEN] = "";
    size_t line_number = 0;
    size_t entry_count = 0;
    size_t actual_count = 0;
    int missing = 0;
    int has_line = 0;
    CupError err;

    memset(&identity, 0, sizeof(identity));
    if (text_is_empty(base_path) || text_is_empty(host_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_join(manifest_path, sizeof(manifest_path), base_path, CUP_MANIFEST_FILENAME);
    if (err == CUP_OK) {
        err = system_open_regular_file(
            manifest_path, &file, &identity, &file_size, &missing);
    }
    if (err != CUP_OK || missing || file == NULL || file_size == 0 ||
        file_size > MAX_PACKAGE_PATH_TABLE_BYTES) {
        if (file != NULL) {
            (void)fclose(file);
        }
        manifest_diagnostic(diagnostics, "package manifest is missing or invalid", 0);
        return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    }

    err = read_manifest_line(file, line, sizeof(line), &line_number, &has_line);
    if (err != CUP_OK || !has_line || strcmp(line, "format=2") != 0) {
        manifest_diagnostic(diagnostics, "package manifest requires format=2", line_number);
        err = CUP_ERR_VALIDATION;
        goto done;
    }

    while (1) {
        char *type;
        char *mode;
        char *digest;
        char *relative;

        if (interrupt_requested()) {
            err = CUP_ERR_INTERRUPT;
            goto done;
        }
        err = read_manifest_line(file, line, sizeof(line), &line_number, &has_line);
        if (err != CUP_OK) {
            manifest_diagnostic(diagnostics, "invalid package manifest syntax", line_number + 1u);
            err = err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
            goto done;
        }
        if (!has_line) {
            break;
        }
        if (entry_count >= MAX_PACKAGE_ARCHIVE_ENTRIES) {
            manifest_diagnostic(diagnostics, "package manifest has too many entries", line_number);
            err = CUP_ERR_VALIDATION;
            goto done;
        }
        if (split_manifest_entry(line, &type, &mode, &digest, &relative) != CUP_OK ||
            !path_is_safe_relative(relative) ||
            (previous[0] != '\0' && strcmp(previous, relative) >= 0)) {
            manifest_diagnostic(diagnostics, "invalid or unsorted package manifest entry", line_number);
            err = CUP_ERR_VALIDATION;
            goto done;
        }
        err = add_folded_path(&path_keys, relative);
        if (err != CUP_OK) {
            manifest_diagnostic(diagnostics, "package manifest contains a path collision", line_number);
            goto done;
        }
        err = verify_manifest_entry(base_path, host_platform, type, mode, digest, relative);
        if (err != CUP_OK) {
            manifest_diagnostic(diagnostics, "package object does not match its manifest entry", line_number);
            goto done;
        }
        if (text_copy(previous, sizeof(previous), relative) != CUP_OK) {
            err = CUP_ERR_VALIDATION;
            goto done;
        }
        entry_count++;
    }

    if (path_keys_have_collision(&path_keys)) {
        manifest_diagnostic(diagnostics, "package manifest contains a case-fold path collision", 0);
        err = CUP_ERR_VALIDATION;
        goto done;
    }
    if (entry_count == 0) {
        manifest_diagnostic(diagnostics, "package manifest has no entries", 0);
        err = CUP_ERR_VALIDATION;
        goto done;
    }
    err = count_package_tree(base_path, manifest_path, &actual_count);
    if (err == CUP_OK && actual_count != entry_count) {
        manifest_diagnostic(diagnostics, "package tree contains missing or undeclared objects", 0);
        err = CUP_ERR_VALIDATION;
    }

done:
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    free_path_keys(&path_keys);
    return err;
}
