/*
 * Exercises portable composite filesystem behavior against the selected native
 * storage stack. Native link traversal remains in each system implementation suite.
 */

#include "error.h"
#include "filesystem.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

void setUp(void);
void tearDown(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];

/* Fixture lifecycle and local construction helpers. */

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

typedef struct {
    const char *text;
    CupError result;
} PublishedText;

static CupError write_published_text(FILE *file, const void *value) {
    const PublishedText *published = value;

    if (file == NULL || published == NULL || published->text == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (published->result != CUP_OK) {
        return published->result;
    }
    return fputs(published->text, file) >= 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

static void assert_file_text(const char *path, const char *expected) {
    char buffer[128];
    FILE *file = fopen(path, "rb");
    size_t size;

    TEST_ASSERT_NOT_NULL(file);
    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    TEST_ASSERT_EQUAL_INT(0, ferror(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buffer[size] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected, buffer);
}

#if defined(_WIN32)
static void change_path_separators(char *path) {
    while (*path != '\0') {
        if (*path == '/') {
            *path = '\\';
        } else if (*path == '\\') {
            *path = '/';
        }
        path++;
    }
}
#endif

/* Test cases grouped by the public contract they exercise. */


static void test_persistent_snapshot(void) {
    PersistentFileSnapshot snapshot;
    char path[1024];
    char missing_path[1024];
    char missing_parent_path[1024];
    char directory_path[1024];
    int missing = 0;

    build_path(path, sizeof(path), "snapshot.txt");
    build_path(missing_path, sizeof(missing_path), "snapshot-missing.txt");
    build_path(missing_parent_path,
               sizeof(missing_parent_path),
               "snapshot-missing-parent/snapshot.txt");
    build_path(directory_path, sizeof(directory_path), "snapshot-directory");
    write_text(path, "format=1\nvalue=test\n");

    filesystem_snapshot_init(&snapshot);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, filesystem_snapshot_read(path, 1024, &snapshot, &missing));
    TEST_ASSERT_FALSE(missing);
    TEST_ASSERT_TRUE(snapshot.identity.valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, snapshot.identity.kind);
    TEST_ASSERT_EQUAL_size_t(strlen("format=1\nvalue=test\n"), snapshot.size);
    TEST_ASSERT_EQUAL_MEMORY("format=1\nvalue=test\n", snapshot.data, snapshot.size);
    filesystem_snapshot_release(&snapshot);
    TEST_ASSERT_NULL(snapshot.data);
    TEST_ASSERT_EQUAL_size_t(0, snapshot.size);

    filesystem_snapshot_init(&snapshot);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, filesystem_snapshot_read(missing_path, 1024, &snapshot, &missing));
    TEST_ASSERT_TRUE(missing);
    TEST_ASSERT_NULL(snapshot.data);

    missing = 0;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, filesystem_snapshot_read(missing_parent_path, 1024, &snapshot, &missing));
    TEST_ASSERT_TRUE(missing);
    TEST_ASSERT_NULL(snapshot.data);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          filesystem_snapshot_read(path, 4, &snapshot, &missing));
    TEST_ASSERT_NULL(snapshot.data);

    TEST_ASSERT_EQUAL_INT(0, test_mkdir(directory_path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          filesystem_snapshot_read(directory_path, 1024, &snapshot, &missing));
    TEST_ASSERT_EQUAL_INT(0, test_rmdir(directory_path));

#if !defined(_WIN32)
    {
        char link_path[1024];
        build_path(link_path, sizeof(link_path), "snapshot-link");
        TEST_ASSERT_EQUAL_INT(0, symlink(path, link_path));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              filesystem_snapshot_read(link_path, 1024, &snapshot, &missing));
        TEST_ASSERT_EQUAL_INT(0, test_unlink(link_path));
    }
    {
        char real_parent[1024];
        char linked_parent[1024];
        char real_file[1024];
        char linked_file[1024];

        build_path(real_parent, sizeof(real_parent), "snapshot-real-parent");
        build_path(linked_parent, sizeof(linked_parent), "snapshot-linked-parent");
        TEST_ASSERT_EQUAL_INT(0, test_mkdir(real_parent, 0755));
        TEST_ASSERT_TRUE(snprintf(real_file, sizeof(real_file), "%s/value", real_parent) > 0);
        write_text(real_file, "data\n");
        TEST_ASSERT_EQUAL_INT(0, symlink(real_parent, linked_parent));
        TEST_ASSERT_TRUE(snprintf(linked_file, sizeof(linked_file), "%s/value", linked_parent) > 0);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              filesystem_snapshot_read(linked_file, 1024, &snapshot, &missing));
        TEST_ASSERT_EQUAL_INT(0, test_unlink(linked_parent));
        TEST_ASSERT_EQUAL_INT(0, test_unlink(real_file));
        TEST_ASSERT_EQUAL_INT(0, test_rmdir(real_parent));
    }
#endif

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_snapshot_read(NULL, 1024, &snapshot, &missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_snapshot_read(path, 0, &snapshot, &missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_snapshot_read(path, 1024, NULL, &missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_snapshot_read(path, 1024, &snapshot, NULL));
    filesystem_snapshot_release(&snapshot);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(path));
}

static void test_atomic_file_publication(void) {
    PublishedText first = {"first\n", CUP_OK};
    PublishedText second = {"second\n", CUP_OK};
    PublishedText failed = {"ignored\n", CUP_ERR_VALIDATION};
    char created[1024];
    char replaced[1024];
    char outside_directory[1024];
    SystemPathIdentity expected_identity;
    SystemPathIdentity stale_identity;
    size_t before;
    size_t after;
    int executable;

#if defined(_WIN32)
    build_path(created, sizeof(created), "published-new.cmd");
#else
    build_path(created, sizeof(created), "published-new");
#endif
    build_path(replaced, sizeof(replaced), "published-replaced");
    TEST_ASSERT_TRUE(snprintf(outside_directory,
                              sizeof(outside_directory),
                              "%s/../published-outside",
                              temp_dir) > 0);
    write_text(replaced, "old\n");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        filesystem_publish_new_file(
            temp_dir, "publish", created, 1, write_published_text, &first));
    assert_file_text(created, "first\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(created, &executable));
    TEST_ASSERT_TRUE(executable);

    TEST_ASSERT_NOT_EQUAL(
        CUP_OK,
        filesystem_publish_new_file(
            temp_dir, "publish", created, 0, write_published_text, &second));
    assert_file_text(created, "first\n");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        filesystem_replace_file_atomically(
            temp_dir, "publish", replaced, 0, write_published_text, &second));
    assert_file_text(replaced, "second\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(replaced, &expected_identity));
    stale_identity = expected_identity;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        filesystem_replace_file_if_identity(
            temp_dir, "publish", replaced, &expected_identity, 0, write_published_text, &first));
    assert_file_text(replaced, "first\n");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        filesystem_replace_file_if_identity(
            temp_dir, "publish", replaced, &stale_identity, 0, write_published_text, &second));
    assert_file_text(replaced, "first\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(temp_dir, NULL, &before));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_VALIDATION,
        filesystem_replace_file_atomically(
            temp_dir, "publish", replaced, 0, write_published_text, &failed));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(temp_dir, NULL, &after));
    TEST_ASSERT_EQUAL_size_t(before, after);
    assert_file_text(replaced, "first\n");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_publish_new_file(
            temp_dir, "publish", outside_directory, 0, write_published_text, &first));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_publish_new_file(
            NULL, "publish", created, 0, write_published_text, &first));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_publish_new_file(
            temp_dir, "", created, 0, write_published_text, &first));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_replace_file_atomically(
            temp_dir, "publish", created, 2, write_published_text, &first));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_replace_file_atomically(
            temp_dir, "publish", created, 0, NULL, &first));
    memset(&expected_identity, 0, sizeof(expected_identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        filesystem_replace_file_if_identity(
            temp_dir, "publish", replaced, &expected_identity, 0, write_published_text, &first));

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(created));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(replaced));
}

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
#if defined(_WIN32)
    write_text(link_path, "link-fixture");
#else
    TEST_ASSERT_EQUAL_INT(0, symlink("nested/file", link_path));
#endif

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

#if !defined(_WIN32)
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
#endif

#if !defined(_WIN32)
static void test_helper_remove_tree_does_not_follow_symlink(void) {
    char external[1024];
    char sentinel[1024];
    char link_path[1024];
    int exists;

    build_path(external, sizeof(external), "helper-external-target");
    build_path(link_path, sizeof(link_path), "helper-external-link");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(external));
    TEST_ASSERT_TRUE(snprintf(sentinel, sizeof(sentinel), "%s/sentinel", external) > 0);
    write_text(sentinel, "preserve");
    TEST_ASSERT_EQUAL_INT(0, symlink(external, link_path));

    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(link_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(link_path, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(sentinel, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(external));
}
#endif

static void test_count_and_clear(void) {
    char root[1024];
    char keep[1024];
#if defined(_WIN32)
    char equivalent_keep[1024];
#endif
    char remove_file[1024];
    char remove_dir[1024];
    char nested[1024];
    char missing[1024];
    size_t count;
    int exists;

    build_path(root, sizeof(root), "clear");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_TRUE(snprintf(keep, sizeof(keep), "%s/keep", root) > 0);
    TEST_ASSERT_TRUE(snprintf(remove_file, sizeof(remove_file), "%s/remove", root) > 0);
    TEST_ASSERT_TRUE(snprintf(remove_dir, sizeof(remove_dir), "%s/remove-dir", root) > 0);
    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", remove_dir) > 0);
    build_path(missing, sizeof(missing), "clear-missing");
    write_text(keep, "keep");
    write_text(remove_file, "remove");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(remove_dir));
    write_text(nested, "nested");

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(root, NULL, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
#if defined(_WIN32)
    TEST_ASSERT_TRUE(snprintf(equivalent_keep, sizeof(equivalent_keep), "%s", keep) > 0);
    change_path_separators(equivalent_keep);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(root, equivalent_keep, &count));
#else
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(root, keep, &count));
#endif
    TEST_ASSERT_EQUAL_size_t(2, count);

#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_clear_directory(root, equivalent_keep));
#else
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_clear_directory(root, keep));
#endif
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(keep, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(remove_file, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(remove_dir, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_count_children(root, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_count_children(NULL, NULL, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_clear_directory(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_count_children(missing, NULL, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_clear_directory(missing, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, filesystem_count_children(keep, NULL, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, filesystem_clear_directory(keep, NULL));
}

static void test_file_policy(void) {
    char path[1024];
    int executable;
    int read_only;

#if defined(_WIN32)
    build_path(path, sizeof(path), "policy.cmd");
#else
    build_path(path, sizeof(path), "policy");
#endif
    write_text(path, "policy");

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_apply_required_permissions(path, 1, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(path, &executable));
    TEST_ASSERT_TRUE(executable);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(path, &read_only));
    TEST_ASSERT_TRUE(read_only);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_apply_required_permissions(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_apply_required_permissions(path, 2, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_apply_required_permissions(path, 0, -1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_apply_required_permissions(path, 0, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(path, &executable));
#if defined(_WIN32)
    TEST_ASSERT_TRUE(executable);
#else
    TEST_ASSERT_FALSE(executable);
#endif
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(path, &read_only));
    TEST_ASSERT_FALSE(read_only);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(path, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(path));
}

static void test_invalid_backup(void) {
    char path[1024];
    char first_candidate[1024];
    char backup[1024];
    char missing[1024];
    char alias_expected[1024];
    int exists;
    int read_only;

    build_path(path, sizeof(path), "invalid-state");
    build_path(missing, sizeof(missing), "invalid-missing");
    TEST_ASSERT_TRUE(snprintf(first_candidate, sizeof(first_candidate), "%s.invalid", path) > 0);
    write_text(path, "state");
    write_text(first_candidate, "older");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(path, 1));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          filesystem_backup_invalid(path, (char[2]){0}, 2));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_backup_invalid(path, backup, sizeof(backup)));
    {
        char expected_backup[1024];

        TEST_ASSERT_TRUE(snprintf(expected_backup, sizeof(expected_backup), "%s.invalid.1", path) > 0);
        TEST_ASSERT_EQUAL_STRING(expected_backup, backup);
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(backup, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(backup, &read_only));
    TEST_ASSERT_TRUE(read_only);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(backup));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          filesystem_backup_invalid(NULL, backup, sizeof(backup)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_backup_invalid(backup, NULL, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, filesystem_backup_invalid(backup, backup, 0));
    TEST_ASSERT_TRUE(snprintf(backup, sizeof(backup), "stale-backup") > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          filesystem_backup_invalid(missing, backup, sizeof(backup)));
    TEST_ASSERT_EQUAL_STRING("", backup);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(first_candidate));

    build_path(path, sizeof(path), "identity-backup");
    write_text(path, "observed\n");
    {
        SystemPathIdentity expected;
        char replacement[1024];

        TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(path, &expected));
        build_path(replacement, sizeof(replacement), "identity-backup-old");
        TEST_ASSERT_EQUAL_INT(0, rename(path, replacement));
        write_text(path, "foreign\n");
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_TRANSACTION,
            filesystem_backup_invalid_if_identity(path, &expected, backup, sizeof(backup)));
        assert_file_text(path, "foreign\n");
        TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(path));
        TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(replacement));
    }

    build_path(path, sizeof(path), "alias-backup");
    TEST_ASSERT_TRUE(snprintf(alias_expected, sizeof(alias_expected), "%s.invalid", path) > 0);
    TEST_ASSERT_TRUE(strlen(alias_expected) < sizeof(alias_expected));
    write_text(path, "alias\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_backup_invalid(path, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(alias_expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, filesystem_remove_tree(path));
}


void register_filesystem_tests(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-filesystem-test"));
    RUN_TEST(test_persistent_snapshot);
    RUN_TEST(test_atomic_file_publication);
    RUN_TEST(test_tree_lifecycle);
#if !defined(_WIN32)
    RUN_TEST(test_remove_symlink_tree);
    RUN_TEST(test_helper_remove_tree_does_not_follow_symlink);
#endif
    RUN_TEST(test_count_and_clear);
    RUN_TEST(test_file_policy);
    RUN_TEST(test_invalid_backup);
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(temp_dir));
}
