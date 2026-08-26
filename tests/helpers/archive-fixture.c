/* Creates deterministic valid or intentionally unsafe package archives. */
#include <archive.h>
#include <archive_entry.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    const char *package_root;
    const char *version;
    const char *host;
    const char *target;
    const char *output;
    const char *mode;
    const char *extra_path;
    const char *extra_content;
} FixtureOptions;

/* Archive entry construction. */
static void fail_archive(struct archive *archive, const char *context) {
    fprintf(stderr,
            "%s: %s\n",
            context,
            archive != NULL ? archive_error_string(archive) : "archive error");
    exit(1);
}

static void add_directory(struct archive *archive, const char *path) {
    struct archive_entry *entry = archive_entry_new();

    if (entry == NULL) {
        fail_archive(archive, "archive_entry_new");
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, 0);
    if (archive_write_header(archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        fail_archive(archive, "write directory");
    }
    archive_entry_free(entry);
}

static void add_file(struct archive *archive,
                     const char *path,
                     const void *data,
                     size_t size,
                     mode_t mode) {
    struct archive_entry *entry = archive_entry_new();

    if (entry == NULL) {
        fail_archive(archive, "archive_entry_new");
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, mode);
    archive_entry_set_size(entry, (la_int64_t)size);
    if (archive_write_header(archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        fail_archive(archive, "write file header");
    }
    if (size > 0 && archive_write_data(archive, data, size) != (la_ssize_t)size) {
        archive_entry_free(entry);
        fail_archive(archive, "write file data");
    }
    archive_entry_free(entry);
}

static void add_link(struct archive *archive, const char *path, const char *target, int hardlink) {
    struct archive_entry *entry = archive_entry_new();

    if (entry == NULL) {
        fail_archive(archive, "archive_entry_new");
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_perm(entry, 0777);
    if (hardlink) {
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_hardlink(entry, target);
    } else {
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_symlink(entry, target);
    }
    archive_entry_set_size(entry, 0);
    if (archive_write_header(archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        fail_archive(archive, "write link");
    }
    archive_entry_free(entry);
}

static void add_fifo(struct archive *archive, const char *path) {
    struct archive_entry *entry = archive_entry_new();

    if (entry == NULL) {
        fail_archive(archive, "archive_entry_new");
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFIFO);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, 0);
    if (archive_write_header(archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        fail_archive(archive, "write fifo");
    }
    archive_entry_free(entry);
}

static void join_path(char *out, size_t size, const char *left, const char *right) {
    int written = snprintf(out, size, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "fixture path is too long\n");
        exit(2);
    }
}

/* Fixture configuration and common package contents. */
static int parse_options(int argc, char **argv, FixtureOptions *options) {
    if ((argc != 7 && argc != 9) || options == NULL) {
        return 0;
    }
    memset(options, 0, sizeof(*options));
    options->package_root = argv[1];
    options->version = argv[2];
    options->host = argv[3];
    options->target = argv[4];
    options->output = argv[5];
    options->mode = argv[6];
    if (argc == 9) {
        options->extra_path = argv[7];
        options->extra_content = argv[8];
    }
    return strcmp(options->mode, "extra-file") == 0 ? argc == 9 : argc == 7;
}

static size_t build_metadata(const FixtureOptions *options, char *info, size_t info_size) {
    int written = snprintf(info,
                           info_size,
                           "package.component=compiler\n"
                           "package.tool=clang\n"
                           "package.version=%s\n"
                           "platform.host=%s\n"
                           "platform.target=%s\n"
                           "entry.clang=%s\n",
                           options->version,
                           options->host,
                           options->target,
                           strcmp(options->target, "windows-x64") == 0
                               ? "bin/clang.cmd"
                               : "bin/clang");

    if (written < 0 || (size_t)written >= info_size) {
        fprintf(stderr, "fixture metadata is too long\n");
        exit(2);
    }
    return (size_t)written;
}

static int ends_with(const char *value, const char *suffix) {
    size_t value_length;
    size_t suffix_length;

    if (value == NULL || suffix == NULL) {
        return 0;
    }
    value_length = strlen(value);
    suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           strcmp(value + value_length - suffix_length, suffix) == 0;
}

static struct archive *open_archive(const char *output) {
    struct archive *archive = archive_write_new();

    if (archive == NULL) {
        fail_archive(NULL, "archive_write_new");
    }
    if (ends_with(output, ".zip")) {
        if (archive_write_set_format_zip(archive) != ARCHIVE_OK) {
            fail_archive(archive, "select ZIP format");
        }
    } else if (archive_write_add_filter_gzip(archive) != ARCHIVE_OK ||
               archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK) {
        fail_archive(archive, "select tar.gz format");
    }
    if (archive_write_open_filename(archive, output) != ARCHIVE_OK) {
        fail_archive(archive, "open output archive");
    }
    return archive;
}

static void add_common_package(struct archive *archive,
                               const FixtureOptions *options,
                               const char *info,
                               size_t info_size) {
    static const char posix_script[] = "#!/bin/sh\nprintf '%s\\n' unsafe\n";
    char windows_script[256];
    char path[1024];
    int windows_package = strcmp(options->target, "windows-x64") == 0;
    int written;

    if (strcmp(options->mode, "root-file") == 0) {
        add_file(archive, options->package_root, "not a directory\n", 16, 0644);
    } else {
        add_directory(archive, options->package_root);
    }
    join_path(path, sizeof(path), options->package_root, "bin");
    add_directory(archive, path);
    join_path(path, sizeof(path), options->package_root, "info.txt");
    add_file(archive, path, info, info_size, 0644);

    if (windows_package) {
        join_path(path, sizeof(path), options->package_root, "bin/clang.cmd");
        written = snprintf(windows_script,
                           sizeof(windows_script),
                           "@echo off\r\necho clang-%s-%s:clang\r\n",
                           options->version,
                           options->target);
        if (written < 0 || (size_t)written >= sizeof(windows_script)) {
            fprintf(stderr, "Windows command fixture is too long\n");
            exit(2);
        }
        add_file(archive, path, windows_script, (size_t)written, 0644);
    } else {
        join_path(path, sizeof(path), options->package_root, "bin/clang");
        add_file(archive, path, posix_script, sizeof(posix_script) - 1, 0755);
    }
}

/* Mode-specific entries model one archive-safety condition per invocation. */
static int add_mode_entries(struct archive *archive, const FixtureOptions *options) {
    char path[1024];

    if (strcmp(options->mode, "valid") == 0) {
        return 1;
    }
    if (strcmp(options->mode, "extra-file") == 0) {
        size_t content_size;

        if (options->extra_path == NULL || options->extra_content == NULL) {
            return 0;
        }
        content_size = strlen(options->extra_content);
        add_file(archive,
                 options->extra_path,
                 options->extra_content,
                 content_size,
                 0644);
        return 1;
    }

    if (strcmp(options->mode, "root-file") == 0) {
        return 1;
    }
    if (strcmp(options->mode, "traversal") == 0) {
        int written = snprintf(path, sizeof(path), "%s/../outside.txt", options->package_root);

        if (written < 0 || (size_t)written >= sizeof(path)) {
            fprintf(stderr, "fixture path is too long\n");
            exit(2);
        }
        add_file(archive, path, "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "absolute") == 0) {
        const char *escape_path = getenv("CUP_TEST_ABSOLUTE_ESCAPE_PATH");

        if (escape_path == NULL || escape_path[0] != '/') {
            fprintf(stderr, "CUP_TEST_ABSOLUTE_ESCAPE_PATH must be absolute\n");
            exit(2);
        }
        add_file(archive, escape_path, "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "symlink") == 0) {
        join_path(path, sizeof(path), options->package_root, "bin/evil-link");
        add_link(archive, path, "../../outside.txt", 0);
        return 1;
    }
    if (strcmp(options->mode, "symlink-parent") == 0) {
        join_path(path, sizeof(path), options->package_root, "real");
        add_directory(archive, path);
        join_path(path, sizeof(path), options->package_root, "bin/redirect");
        add_link(archive, path, "../real", 0);
        join_path(path, sizeof(path), options->package_root, "bin/redirect/child");
        add_file(archive, path, "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "duplicate") == 0) {
        join_path(path, sizeof(path), options->package_root, "bin/clang");
        add_file(archive, path, "overwrite\n", 10, 0755);
        return 1;
    }
    if (strcmp(options->mode, "case-collision") == 0) {
        join_path(path, sizeof(path), options->package_root, "bin/CLANG");
        add_file(archive, path, "collision\n", 10, 0755);
        return 1;
    }
    if (strcmp(options->mode, "file-directory") == 0) {
        join_path(path, sizeof(path), options->package_root, "conflict");
        add_file(archive, path, "file\n", 5, 0644);
        join_path(path, sizeof(path), options->package_root, "conflict/child");
        add_file(archive, path, "child\n", 6, 0644);
        return 1;
    }
    if (strcmp(options->mode, "reserved") == 0) {
        join_path(path, sizeof(path), options->package_root, "bin/CON");
        add_file(archive, path, "reserved\n", 9, 0644);
        return 1;
    }
    if (strcmp(options->mode, "unicode") == 0) {
        join_path(path, sizeof(path), options->package_root, "bin/caf\303\251");
        add_file(archive, path, "unicode\n", 8, 0644);
        return 1;
    }
    if (strcmp(options->mode, "special") == 0) {
        join_path(path, sizeof(path), options->package_root, "pipe");
        add_fifo(archive, path);
        return 1;
    }
    if (strcmp(options->mode, "hardlink-forward") == 0) {
        char target_path[1024];

        join_path(path, sizeof(path), options->package_root, "bin/copy");
        join_path(target_path, sizeof(target_path), options->package_root, "bin/later");
        add_link(archive, path, target_path, 1);
        add_file(archive, target_path, "later\n", 6, 0644);
        return 1;
    }
    return 0;
}

/* libarchive's native Windows ZIP writer rewrites '\\' pathname separators to '/'.
 * Restore the deliberately hostile spelling in the ZIP filename fields, then verify it
 * again through libarchive before CUP consumes the archive. */
static int zip_name_field(const unsigned char *data,
                          size_t data_size,
                          size_t name_offset,
                          size_t name_size,
                          int central_directory) {
    static const unsigned char local_signature[] = {0x50, 0x4b, 0x03, 0x04};
    static const unsigned char central_signature[] = {0x50, 0x4b, 0x01, 0x02};
    const unsigned char *signature = central_directory ? central_signature : local_signature;
    size_t header_size = central_directory ? 46 : 30;
    size_t name_length_offset = central_directory ? 28 : 26;
    size_t header_offset;
    unsigned int stored_name_size;

    if (name_offset < header_size || name_size > 0xffff) {
        return 0;
    }
    header_offset = name_offset - header_size;
    if (header_offset + header_size > data_size ||
        memcmp(data + header_offset, signature, sizeof(local_signature)) != 0) {
        return 0;
    }
    stored_name_size = (unsigned int)data[header_offset + name_length_offset] |
                       ((unsigned int)data[header_offset + name_length_offset + 1] << 8);
    return stored_name_size == name_size;
}

static int restore_zip_backslash_path(const char *archive_path, const char *expected) {
    FILE *file = NULL;
    unsigned char *data = NULL;
    char *normalized = NULL;
    long file_size;
    size_t path_size;
    size_t offset;
    size_t local_offset = 0;
    size_t central_offset = 0;
    int local_found = 0;
    int central_found = 0;
    int result = 0;

    if (!ends_with(archive_path, ".zip") || expected == NULL || strchr(expected, '\\') == NULL) {
        return 0;
    }

    path_size = strlen(expected);
    normalized = malloc(path_size + 1);
    if (normalized == NULL) {
        goto cleanup;
    }
    memcpy(normalized, expected, path_size + 1);
    for (offset = 0; offset < path_size; ++offset) {
        if (normalized[offset] == '\\') {
            normalized[offset] = '/';
        }
    }

    file = fopen(archive_path, "rb+");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        goto cleanup;
    }
    data = malloc((size_t)file_size);
    if (data == NULL || fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        goto cleanup;
    }

    if (path_size > (size_t)file_size) {
        goto cleanup;
    }
    for (offset = 0; offset <= (size_t)file_size - path_size; ++offset) {
        if (memcmp(data + offset, normalized, path_size) != 0) {
            continue;
        }
        if (zip_name_field(data, (size_t)file_size, offset, path_size, 0)) {
            if (local_found) {
                goto cleanup;
            }
            local_offset = offset;
            local_found = 1;
        } else if (zip_name_field(data, (size_t)file_size, offset, path_size, 1)) {
            if (central_found) {
                goto cleanup;
            }
            central_offset = offset;
            central_found = 1;
        }
    }
    if (!local_found || !central_found) {
        goto cleanup;
    }

    for (offset = 0; offset < path_size; ++offset) {
        if (expected[offset] == '\\') {
            data[local_offset + offset] = '\\';
            data[central_offset + offset] = '\\';
        }
    }
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(data, 1, (size_t)file_size, file) != (size_t)file_size || fflush(file) != 0) {
        goto cleanup;
    }
    result = 1;

cleanup:
    free(data);
    free(normalized);
    if (file != NULL && fclose(file) != 0) {
        result = 0;
    }
    return result;
}

static int archive_contains_exact_path(const char *archive_path, const char *expected) {
    struct archive *reader = archive_read_new();
    struct archive_entry *entry = NULL;
    int found = 0;
    int status;

    if (reader == NULL) {
        return 0;
    }
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_filename(reader, archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(reader);
        return 0;
    }
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);

        if (pathname != NULL && strcmp(pathname, expected) == 0) {
            found = 1;
        }
        if (archive_read_data_skip(reader) != ARCHIVE_OK) {
            found = 0;
            break;
        }
    }
    if (status != ARCHIVE_EOF) {
        found = 0;
    }
    if (archive_read_close(reader) != ARCHIVE_OK) {
        found = 0;
    }
    if (archive_read_free(reader) != ARCHIVE_OK) {
        found = 0;
    }
    return found;
}

static int close_archive(struct archive *archive) {
    if (archive_write_close(archive) != ARCHIVE_OK) {
        fail_archive(archive, "close archive");
    }
    if (archive_write_free(archive) != ARCHIVE_OK) {
        fprintf(stderr, "failed to free archive writer\n");
        return 0;
    }
    return 1;
}

/* Process entry point. */
int main(int argc, char **argv) {
    FixtureOptions options;
    struct archive *archive;
    char info[1024];
    size_t info_size;

    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr,
                "usage: %s PACKAGE_ROOT VERSION HOST TARGET OUTPUT MODE "
                "[EXTRA_PATH EXTRA_CONTENT]\n",
                argv[0]);
        return 2;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") == NULL &&
        setlocale(LC_CTYPE, "en_US.UTF-8") == NULL &&
        setlocale(LC_CTYPE, ".UTF-8") == NULL) {
        fprintf(stderr, "a UTF-8 locale is required to create archive fixtures\n");
        return 1;
    }

    info_size = build_metadata(&options, info, sizeof(info));
    archive = open_archive(options.output);
    add_common_package(archive, &options, info, info_size);
    if (!add_mode_entries(archive, &options)) {
        fprintf(stderr, "unknown fixture mode: %s\n", options.mode);
        (void)archive_write_close(archive);
        (void)archive_write_free(archive);
        return 2;
    }
    if (!close_archive(archive)) {
        return 1;
    }
    if (options.extra_path != NULL &&
        !archive_contains_exact_path(options.output, options.extra_path)) {
        if (!restore_zip_backslash_path(options.output, options.extra_path) ||
            !archive_contains_exact_path(options.output, options.extra_path)) {
            fprintf(stderr,
                    "archive fixture did not preserve exact entry name: %s\n",
                    options.extra_path);
            return 1;
        }
    }
    return 0;
}
