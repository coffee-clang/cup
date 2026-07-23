/*
 * Test focus: Exercises archive-entry safety and extraction normalization with real libarchive
 * files and no package-install workflow.
 */

#include "package_extract.h"

#include "constants.h"
#include "interrupt.h"
#include "package_archive.h"
#include "unity.h"
#include "test_platform.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    TEST_FILE,
    TEST_DIRECTORY,
    TEST_SYMLINK,
    TEST_HARDLINK,
    TEST_FIFO
} TestEntryType;

typedef struct {
    TestEntryType type;
    const char *path;
    const char *content;
    const char *link;
    int executable;
} TestEntry;

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char root[1024];
static int interrupted;
static unsigned archive_number;

/* Fixture lifecycle and local construction helpers. */

static void join_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        TEST_ASSERT_TRUE(unlink(path) == 0 || errno == ENOENT);
        return;
    }

    while ((entry = readdir(directory)) != NULL) {
        char child[1200];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        join_path(child, sizeof(child), path, entry->d_name);
        TEST_ASSERT_EQUAL_INT(0, lstat(child, &status));
        if (S_ISDIR(status.st_mode)) {
            remove_tree(child);
        } else {
            TEST_ASSERT_EQUAL_INT(0, unlink(child));
        }
    }

    TEST_ASSERT_EQUAL_INT(0, closedir(directory));
    TEST_ASSERT_EQUAL_INT(0, rmdir(path));
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(test_mkdir(path, 0700) == 0 || errno == EEXIST);
}

void setUp(void) {
    char template_path[] = "/tmp/cup-extract-unit-XXXXXX";

    TEST_ASSERT_NOT_NULL(mkdtemp(template_path));
    strcpy(root, template_path);
    interrupted = 0;
    archive_number = 0;
}

void tearDown(void) {
    remove_tree(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

int interrupt_requested(void) {
    return interrupted;
}

int package_archive_reader_matches_format(struct archive *reader, PackageArchiveFormat expected) {
    (void)reader;
    return expected == PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
}

CupError package_archive_open_reader(struct archive **reader, const char *archive_path) {
    struct archive *candidate = archive_read_new();

    if (candidate == NULL) {
        return CUP_ERR_ARCHIVE;
    }
    archive_read_support_filter_all(candidate);
    archive_read_support_format_all(candidate);
    if (archive_read_open_filename(candidate, archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(candidate);
        return CUP_ERR_ARCHIVE;
    }
    *reader = candidate;
    return CUP_OK;
}

static void create_archive(char *path, size_t path_size, const TestEntry *entries, size_t count) {
    struct archive *writer = archive_write_new();
    size_t i;

    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_set_format_pax_restricted(writer));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_add_filter_gzip(writer));
    TEST_ASSERT_TRUE(snprintf(path, path_size, "%s/archive-%u.tar.gz", root, ++archive_number) > 0);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_open_filename(writer, path));

    for (i = 0; i < count; ++i) {
        const TestEntry *spec = &entries[i];
        struct archive_entry *entry = archive_entry_new();
        size_t content_size = spec->content == NULL ? 0 : strlen(spec->content);

        TEST_ASSERT_NOT_NULL(entry);
        archive_entry_set_pathname(entry, spec->path);
        archive_entry_set_uid(entry, 1000);
        archive_entry_set_gid(entry, 1000);
        archive_entry_set_mtime(entry, 1, 0);

        if (spec->type == TEST_DIRECTORY) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0700);
            archive_entry_set_size(entry, 0);
        } else if (spec->type == TEST_SYMLINK) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_perm(entry, 0777);
            archive_entry_set_symlink(entry, spec->link);
            archive_entry_set_size(entry, 0);
        } else if (spec->type == TEST_HARDLINK) {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_hardlink(entry, spec->link);
            archive_entry_set_size(entry, 0);
        } else if (spec->type == TEST_FIFO) {
            archive_entry_set_filetype(entry, AE_IFIFO);
            archive_entry_set_perm(entry, 0600);
            archive_entry_set_size(entry, 0);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, spec->executable ? 0711 : 0600);
            archive_entry_set_size(entry, (la_int64_t)content_size);
        }

        {
            int header_status = archive_write_header(writer, entry);
            TEST_ASSERT_TRUE(header_status >= ARCHIVE_WARN);
        }
        if (spec->type == TEST_FILE && content_size > 0) {
            TEST_ASSERT_EQUAL_INT((int)content_size,
                                  (int)archive_write_data(writer, spec->content, content_size));
        }
        archive_entry_free(entry);
    }

    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_close(writer));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_free(writer));
}

static CupError extract_entries(const TestEntry *entries,
                                size_t count,
                                char *destination,
                                size_t destination_size) {
    char archive_path[1024];

    create_archive(archive_path, sizeof(archive_path), entries, count);
    TEST_ASSERT_TRUE(snprintf(destination, destination_size, "%s/output-%u", root, archive_number) >
                     0);
    make_dir(destination);
    return package_extract_archive(archive_path, destination, "tar.gz");
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_invalid_inputs(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_extract_archive(NULL, "/tmp", "tar.gz"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_extract_archive("", "/tmp", "tar.gz"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_extract_archive("/tmp/archive", "", "tar.gz"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE,
                          package_extract_archive("/missing/archive.tar.gz", "/tmp", "tar.gz"));
}

static void test_valid_archive(void) {
    const TestEntry entries[] = {
        {TEST_DIRECTORY, "pkg/", NULL, NULL, 0},
        {TEST_DIRECTORY, "pkg/bin/", NULL, NULL, 0},
        {TEST_FILE, "pkg/bin/tool", "hello", NULL, 1},
        {TEST_FILE, "pkg/readme", "text", NULL, 0},
        {TEST_SYMLINK, "pkg/bin/current", NULL, "tool", 0},
        {TEST_HARDLINK, "pkg/bin/copy", NULL, "pkg/bin/tool", 0},
        {TEST_SYMLINK, "pkg/current", NULL, "tool", 0},
        {TEST_SYMLINK, "pkg/bin/readme-link", NULL, "../readme", 0},
    };
    char output[1024];
    char tool[1024];
    char readme[1024];
    char current[1024];
    char copy[1024];
    char root_link[1024];
    char parent_link[1024];
    struct stat tool_stat;
    struct stat copy_stat;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        extract_entries(entries, sizeof(entries) / sizeof(entries[0]), output, sizeof(output)));
    join_path(tool, sizeof(tool), output, "bin/tool");
    join_path(readme, sizeof(readme), output, "readme");
    join_path(current, sizeof(current), output, "bin/current");
    join_path(copy, sizeof(copy), output, "bin/copy");
    join_path(root_link, sizeof(root_link), output, "current");
    join_path(parent_link, sizeof(parent_link), output, "bin/readme-link");
    TEST_ASSERT_EQUAL_INT(0, stat(tool, &tool_stat));
    TEST_ASSERT_TRUE((tool_stat.st_mode & S_IXUSR) != 0);
    TEST_ASSERT_EQUAL_INT(0, stat(readme, &copy_stat));
    TEST_ASSERT_FALSE((copy_stat.st_mode & S_IXUSR) != 0);
    TEST_ASSERT_EQUAL_INT(0, lstat(current, &copy_stat));
    TEST_ASSERT_TRUE(S_ISLNK(copy_stat.st_mode));
    TEST_ASSERT_EQUAL_INT(0, stat(copy, &copy_stat));
    TEST_ASSERT_TRUE(tool_stat.st_ino == copy_stat.st_ino);
    TEST_ASSERT_EQUAL_INT(0, lstat(root_link, &copy_stat));
    TEST_ASSERT_TRUE(S_ISLNK(copy_stat.st_mode));
    TEST_ASSERT_EQUAL_INT(0, lstat(parent_link, &copy_stat));
    TEST_ASSERT_TRUE(S_ISLNK(copy_stat.st_mode));
}

static void test_unsafe_paths(void) {
    const TestEntry absolute[] = {
        {TEST_FILE, "/pkg/tool", "x", NULL, 0},
    };
    const TestEntry traversal[] = {
        {TEST_FILE, "pkg/../tool", "x", NULL, 0},
    };
    const TestEntry roots[] = {
        {TEST_FILE, "one/tool", "x", NULL, 0},
        {TEST_FILE, "two/tool", "x", NULL, 0},
    };
    const TestEntry duplicate[] = {
        {TEST_FILE, "pkg/tool", "x", NULL, 0},
        {TEST_FILE, "pkg/tool", "y", NULL, 0},
    };
    const TestEntry drive[] = {
        {TEST_FILE, "C:/pkg/tool", "x", NULL, 0},
    };
    const TestEntry backslash[] = {
        {TEST_FILE, "pkg\\tool", "x", NULL, 0},
    };
    TestEntry deep = {TEST_FILE, NULL, "x", NULL, 0};
    char deep_path[1024] = "pkg";
    char output[1024];
    size_t i;

    for (i = 0; i <= MAX_PACKAGE_PATH_DEPTH; ++i) {
        TEST_ASSERT_TRUE(strcat(deep_path, "/x") != NULL);
    }
    deep.path = deep_path;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(absolute, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(traversal, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(roots, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(duplicate, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(drive, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(backslash, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(&deep, 1, output, sizeof(output)));
}

static void test_unsafe_links(void) {
    const TestEntry symlink[] = {
        {TEST_FILE, "pkg/bin/tool", "x", NULL, 1},
        {TEST_SYMLINK, "pkg/bin/out", NULL, "../../escape", 0},
    };
    const TestEntry hardlink[] = {
        {TEST_HARDLINK, "pkg/bin/copy", NULL, "pkg/bin/tool", 0},
        {TEST_FILE, "pkg/bin/tool", "x", NULL, 1},
    };
    const TestEntry fifo[] = {
        {TEST_FIFO, "pkg/pipe", NULL, NULL, 0},
    };
    char output[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(symlink, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(hardlink, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE, extract_entries(fifo, 1, output, sizeof(output)));
}

static void test_path_collisions(void) {
    const TestEntry case_collision[] = {
        {TEST_FILE, "pkg/bin/Tool", "x", NULL, 1},
        {TEST_FILE, "pkg/bin/tool", "y", NULL, 1},
    };
    const TestEntry file_then_child[] = {
        {TEST_FILE, "pkg/share", "x", NULL, 0},
        {TEST_FILE, "pkg/share/file", "y", NULL, 0},
    };
    const TestEntry child_then_file[] = {
        {TEST_FILE, "pkg/share/file", "y", NULL, 0},
        {TEST_FILE, "pkg/share", "x", NULL, 0},
    };
    const TestEntry symlink_parent[] = {
        {TEST_DIRECTORY, "pkg/real", NULL, NULL, 0},
        {TEST_SYMLINK, "pkg/link", NULL, "real", 0},
        {TEST_FILE, "pkg/link/tool", "x", NULL, 1},
    };
    const TestEntry reserved[] = {
        {TEST_FILE, "pkg/CON.txt", "x", NULL, 0},
    };
    const TestEntry non_ascii[] = {
        {TEST_FILE, "pkg/caf\303\251", "x", NULL, 0},
    };
    char output[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(case_collision, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(file_then_child, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(child_then_file, 2, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(symlink_parent, 3, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(reserved, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(non_ascii, 1, output, sizeof(output)));
}

static void test_empty_archive(void) {
    const TestEntry root_only[] = {
        {TEST_DIRECTORY, "pkg/", NULL, NULL, 0},
    };
    const TestEntry root_file[] = {
        {TEST_FILE, "pkg", "x", NULL, 0},
    };
    char output[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, extract_entries(NULL, 0, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, extract_entries(root_only, 1, output, sizeof(output)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE,
                          extract_entries(root_file, 1, output, sizeof(output)));
}

static void test_interrupt(void) {
    const TestEntry entries[] = {
        {TEST_FILE, "pkg/tool", "hello", NULL, 1},
    };
    char archive_path[1024];
    char output[1024];

    create_archive(archive_path, sizeof(archive_path), entries, 1);
    TEST_ASSERT_TRUE(snprintf(output, sizeof(output), "%s/interrupted", root) > 0);
    make_dir(output);
    interrupted = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_extract_archive(archive_path, output, "tar.gz"));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_inputs);
    RUN_TEST(test_valid_archive);
    RUN_TEST(test_unsafe_paths);
    RUN_TEST(test_unsafe_links);
    RUN_TEST(test_path_collisions);
    RUN_TEST(test_empty_archive);
    RUN_TEST(test_interrupt);
    return UNITY_END();
}
