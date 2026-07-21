/* Creates deterministic valid or intentionally unsafe tar.gz package fixtures. */
#include <archive.h>
#include <archive_entry.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail_archive(struct archive *archive, const char *context) {
    fprintf(stderr,
            "%s: %s\n",
            context,
            archive != NULL ? archive_error_string(archive) : "archive error");
    exit(1);
}

static void add_directory(struct archive *archive, const char *path) {
    struct archive_entry *entry = archive_entry_new();
    if (entry == NULL)
        fail_archive(archive, "archive_entry_new");
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

static void add_file(
    struct archive *archive, const char *path, const void *data, size_t size, int mode) {
    struct archive_entry *entry = archive_entry_new();
    if (entry == NULL)
        fail_archive(archive, "archive_entry_new");
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
    if (entry == NULL)
        fail_archive(archive, "archive_entry_new");
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
    if (entry == NULL)
        fail_archive(archive, "archive_entry_new");
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

int main(int argc, char **argv) {
    struct archive *archive;
    const char *package;
    const char *version;
    const char *host;
    const char *target;
    const char *output;
    const char *mode;
    char info[1024];
    char path[1024];
    int written;
    static const char script[] = "#!/bin/sh\nprintf '%s\\n' unsafe\n";

    if (argc != 7) {
        fprintf(stderr, "usage: %s PACKAGE VERSION HOST TARGET OUTPUT MODE\n", argv[0]);
        return 2;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") == NULL && setlocale(LC_CTYPE, "en_US.UTF-8") == NULL) {
        fprintf(stderr, "a UTF-8 locale is required to create archive fixtures\n");
        return 1;
    }

    package = argv[1];
    version = argv[2];
    host = argv[3];
    target = argv[4];
    output = argv[5];
    mode = argv[6];

    written = snprintf(info,
                       sizeof(info),
                       "package.component=compiler\n"
                       "package.tool=clang\n"
                       "package.version=%s\n"
                       "platform.host=%s\n"
                       "platform.target=%s\n"
                       "entry.clang=bin/clang\n",
                       version,
                       host,
                       target);
    if (written < 0 || (size_t)written >= sizeof(info)) {
        fprintf(stderr, "fixture metadata is too long\n");
        return 2;
    }

    archive = archive_write_new();
    if (archive == NULL)
        fail_archive(NULL, "archive_write_new");
    if (archive_write_add_filter_gzip(archive) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        archive_write_open_filename(archive, output) != ARCHIVE_OK) {
        fail_archive(archive, "open output archive");
    }

    if (strcmp(mode, "root-file") == 0) {
        add_file(archive, package, "not a directory\n", 16, 0644);
    } else {
        add_directory(archive, package);
    }
    join_path(path, sizeof(path), package, "bin");
    add_directory(archive, path);
    join_path(path, sizeof(path), package, "info.txt");
    add_file(archive, path, info, (size_t)written, 0644);
    join_path(path, sizeof(path), package, "bin/clang");
    add_file(archive, path, script, sizeof(script) - 1, 0755);

    if (strcmp(mode, "traversal") == 0) {
        snprintf(path, sizeof(path), "%s/../outside.txt", package);
        add_file(archive, path, "escape\n", 7, 0644);
    } else if (strcmp(mode, "absolute") == 0) {
        add_file(archive, "/tmp/cup-absolute-escape.txt", "escape\n", 7, 0644);
    } else if (strcmp(mode, "symlink") == 0) {
        join_path(path, sizeof(path), package, "bin/evil-link");
        add_link(archive, path, "../../outside.txt", 0);
    } else if (strcmp(mode, "symlink-parent") == 0) {
        join_path(path, sizeof(path), package, "real");
        add_directory(archive, path);
        join_path(path, sizeof(path), package, "bin/redirect");
        add_link(archive, path, "../real", 0);
        join_path(path, sizeof(path), package, "bin/redirect/child");
        add_file(archive, path, "escape\n", 7, 0644);
    } else if (strcmp(mode, "duplicate") == 0) {
        join_path(path, sizeof(path), package, "bin/clang");
        add_file(archive, path, "overwrite\n", 10, 0755);
    } else if (strcmp(mode, "case-collision") == 0) {
        join_path(path, sizeof(path), package, "bin/CLANG");
        add_file(archive, path, "collision\n", 10, 0755);
    } else if (strcmp(mode, "file-directory") == 0) {
        join_path(path, sizeof(path), package, "conflict");
        add_file(archive, path, "file\n", 5, 0644);
        join_path(path, sizeof(path), package, "conflict/child");
        add_file(archive, path, "child\n", 6, 0644);
    } else if (strcmp(mode, "reserved") == 0) {
        join_path(path, sizeof(path), package, "bin/CON");
        add_file(archive, path, "reserved\n", 9, 0644);
    } else if (strcmp(mode, "unicode") == 0) {
        join_path(path, sizeof(path), package, "bin/caf\303\251");
        add_file(archive, path, "unicode\n", 8, 0644);
    } else if (strcmp(mode, "special") == 0) {
        join_path(path, sizeof(path), package, "pipe");
        add_fifo(archive, path);
    } else if (strcmp(mode, "hardlink-forward") == 0) {
        join_path(path, sizeof(path), package, "bin/copy");
        {
            char target_path[1024];
            join_path(target_path, sizeof(target_path), package, "bin/later");
            add_link(archive, path, target_path, 1);
            add_file(archive, target_path, "later\n", 6, 0644);
        }
    } else if (strcmp(mode, "root-file") != 0) {
        fprintf(stderr, "unknown fixture mode: %s\n", mode);
        archive_write_close(archive);
        archive_write_free(archive);
        return 2;
    }

    if (archive_write_close(archive) != ARCHIVE_OK) {
        fail_archive(archive, "close archive");
    }
    if (archive_write_free(archive) != ARCHIVE_OK) {
        fprintf(stderr, "failed to free archive writer\n");
        return 1;
    }
    return 0;
}
