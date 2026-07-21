/*
 * Test focus: Exercises recursive composite filesystem behavior against the concrete POSIX
 * storage stack.
 */

#include "error.h"
#include "filesystem.h"
#include "system.h"
#include "unity.h"

void setUp(void);
void tearDown(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[] = "/tmp/cup-filesystem-test-XXXXXX";

/* Fixture lifecycle and local construction helpers. */

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Test cases grouped by the public contract they exercise. */

static void test_tree_lifecycle(void) {
    char root[1024];
    char nested[1024];
    char file_path[1024];
    char link_path[1024];
    int exists;

    build_path(root, sizeof(root), "tree");
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_ensure_directory(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_ensure_directory(root));

    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", root) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(nested));
    TEST_ASSERT_TRUE(snprintf(file_path, sizeof(file_path), "%s/file", nested) > 0);
    write_text(file_path, "data");
    TEST_ASSERT_TRUE(snprintf(link_path, sizeof(link_path), "%s/link", root) > 0);
    TEST_ASSERT_EQUAL_INT(0, symlink("nested/file", link_path));

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(root, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(root));

    build_path(file_path, sizeof(file_path), "plain-file");
    write_text(file_path, "plain");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, filesystem_ensure_directory(file_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(file_path));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_ensure_directory(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_remove_tree(""));
}

static void test_remove_symlink_tree(void) {
    char root[1024];
    char external[1024];
    char sentinel[1024];
    char link_path[1024];
    int exists;

    build_path(root, sizeof(root), "symlink-tree");
    build_path(external, sizeof(external), "external-target");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(external));
    TEST_ASSERT_TRUE(snprintf(sentinel, sizeof(sentinel), "%s/sentinel", external) > 0);
    write_text(sentinel, "preserve");
    TEST_ASSERT_TRUE(snprintf(link_path, sizeof(link_path), "%s/external", root) > 0);
    TEST_ASSERT_EQUAL_INT(0, symlink(external, link_path));

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(root, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(sentinel, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(external));
}

static void test_count_and_clear(void) {
    char root[1024];
    char keep[1024];
    char remove_file[1024];
    char remove_dir[1024];
    char nested[1024];
    size_t count;
    int exists;

    build_path(root, sizeof(root), "clear");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_TRUE(snprintf(keep, sizeof(keep), "%s/keep", root) > 0);
    TEST_ASSERT_TRUE(snprintf(remove_file, sizeof(remove_file), "%s/remove", root) > 0);
    TEST_ASSERT_TRUE(snprintf(remove_dir, sizeof(remove_dir), "%s/remove-dir", root) > 0);
    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", remove_dir) > 0);
    write_text(keep, "keep");
    write_text(remove_file, "remove");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(remove_dir));
    write_text(nested, "nested");

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(root, NULL, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(root, keep, &count));
    TEST_ASSERT_EQUAL_size_t(2, count);

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_clear_directory(root, keep));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(keep, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(remove_file, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(remove_dir, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_count_children(root, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, filesystem_count_children(keep, NULL, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, filesystem_clear_directory(keep, NULL));
}

static void test_invalid_backup(void) {
    char path[1024];
    char first_candidate[1024];
    char backup[1024];
    int exists;
    int read_only;

    build_path(path, sizeof(path), "invalid-state");
    TEST_ASSERT_TRUE(snprintf(first_candidate, sizeof(first_candidate), "%s.invalid", path) > 0);
    write_text(path, "state");
    write_text(first_candidate, "older");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(path, 1));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          filesystem_backup_invalid(path, (char[2]){0}, 2));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_backup_invalid(path, backup, sizeof(backup)));
    TEST_ASSERT_TRUE(strstr(backup, ".invalid.1") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(backup, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(backup, &read_only));
    TEST_ASSERT_FALSE(read_only);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_backup_invalid(NULL, backup, sizeof(backup)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_backup_invalid(backup, NULL, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(backup));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(first_candidate));
}

/* Suite registration. */

void register_filesystem_tests(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    RUN_TEST(test_tree_lifecycle);
    RUN_TEST(test_remove_symlink_tree);
    RUN_TEST(test_count_and_clear);
    RUN_TEST(test_invalid_backup);
}
