/* Creates deterministic valid or intentionally unsafe tar.gz package fixtures. */
#include <archive.h>
#include <archive_entry.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    const char *package;
    const char *version;
    const char *host;
    const char *target;
    const char *output;
    const char *mode;
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
    if (argc != 7 || options == NULL) {
        return 0;
    }
    options->package = argv[1];
    options->version = argv[2];
    options->host = argv[3];
    options->target = argv[4];
    options->output = argv[5];
    options->mode = argv[6];
    return 1;
}

static size_t build_metadata(const FixtureOptions *options, char *info, size_t info_size) {
    int written = snprintf(info,
                           info_size,
                           "package.component=compiler\n"
                           "package.tool=clang\n"
                           "package.version=%s\n"
                           "platform.host=%s\n"
                           "platform.target=%s\n"
                           "entry.clang=bin/clang\n",
                           options->version,
                           options->host,
                           options->target);

    if (written < 0 || (size_t)written >= info_size) {
        fprintf(stderr, "fixture metadata is too long\n");
        exit(2);
    }
    return (size_t)written;
}

static struct archive *open_archive(const char *output) {
    struct archive *archive = archive_write_new();

    if (archive == NULL) {
        fail_archive(NULL, "archive_write_new");
    }
    if (archive_write_add_filter_gzip(archive) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        archive_write_open_filename(archive, output) != ARCHIVE_OK) {
        fail_archive(archive, "open output archive");
    }
    return archive;
}

static void add_common_package(struct archive *archive,
                               const FixtureOptions *options,
                               const char *info,
                               size_t info_size) {
    static const char script[] = "#!/bin/sh\nprintf '%s\\n' unsafe\n";
    char path[1024];

    if (strcmp(options->mode, "root-file") == 0) {
        add_file(archive, options->package, "not a directory\n", 16, 0644);
    } else {
        add_directory(archive, options->package);
    }
    join_path(path, sizeof(path), options->package, "bin");
    add_directory(archive, path);
    join_path(path, sizeof(path), options->package, "info.txt");
    add_file(archive, path, info, info_size, 0644);
    join_path(path, sizeof(path), options->package, "bin/clang");
    add_file(archive, path, script, sizeof(script) - 1, 0755);
}

/* Mode-specific entries model one archive-safety condition per invocation. */
static int add_mode_entries(struct archive *archive, const FixtureOptions *options) {
    char path[1024];

    if (strcmp(options->mode, "root-file") == 0) {
        return 1;
    }
    if (strcmp(options->mode, "traversal") == 0) {
        int written = snprintf(path, sizeof(path), "%s/../outside.txt", options->package);

        if (written < 0 || (size_t)written >= sizeof(path)) {
            fprintf(stderr, "fixture path is too long\n");
            exit(2);
        }
        add_file(archive, path, "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "absolute") == 0) {
        add_file(archive, "/tmp/cup-absolute-escape.txt", "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "symlink") == 0) {
        join_path(path, sizeof(path), options->package, "bin/evil-link");
        add_link(archive, path, "../../outside.txt", 0);
        return 1;
    }
    if (strcmp(options->mode, "symlink-parent") == 0) {
        join_path(path, sizeof(path), options->package, "real");
        add_directory(archive, path);
        join_path(path, sizeof(path), options->package, "bin/redirect");
        add_link(archive, path, "../real", 0);
        join_path(path, sizeof(path), options->package, "bin/redirect/child");
        add_file(archive, path, "escape\n", 7, 0644);
        return 1;
    }
    if (strcmp(options->mode, "duplicate") == 0) {
        join_path(path, sizeof(path), options->package, "bin/clang");
        add_file(archive, path, "overwrite\n", 10, 0755);
        return 1;
    }
    if (strcmp(options->mode, "case-collision") == 0) {
        join_path(path, sizeof(path), options->package, "bin/CLANG");
        add_file(archive, path, "collision\n", 10, 0755);
        return 1;
    }
    if (strcmp(options->mode, "file-directory") == 0) {
        join_path(path, sizeof(path), options->package, "conflict");
        add_file(archive, path, "file\n", 5, 0644);
        join_path(path, sizeof(path), options->package, "conflict/child");
        add_file(archive, path, "child\n", 6, 0644);
        return 1;
    }
    if (strcmp(options->mode, "reserved") == 0) {
        join_path(path, sizeof(path), options->package, "bin/CON");
        add_file(archive, path, "reserved\n", 9, 0644);
        return 1;
    }
    if (strcmp(options->mode, "unicode") == 0) {
        join_path(path, sizeof(path), options->package, "bin/caf\303\251");
        add_file(archive, path, "unicode\n", 8, 0644);
        return 1;
    }
    if (strcmp(options->mode, "special") == 0) {
        join_path(path, sizeof(path), options->package, "pipe");
        add_fifo(archive, path);
        return 1;
    }
    if (strcmp(options->mode, "hardlink-forward") == 0) {
        char target_path[1024];

        join_path(path, sizeof(path), options->package, "bin/copy");
        join_path(target_path, sizeof(target_path), options->package, "bin/later");
        add_link(archive, path, target_path, 1);
        add_file(archive, target_path, "later\n", 6, 0644);
        return 1;
    }
    return 0;
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
        fprintf(stderr, "usage: %s PACKAGE VERSION HOST TARGET OUTPUT MODE\n", argv[0]);
        return 2;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") == NULL && setlocale(LC_CTYPE, "en_US.UTF-8") == NULL) {
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
    return close_archive(archive) ? 0 : 1;
}
