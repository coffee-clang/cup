/*
 * Test focus: Exercises the native Windows system contract without duplicating
 * command-level integration workflows.
 */

#include "error.h"
#include "path.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static char original_profile[CUP_TEST_TEMP_PATH_SIZE];
static int had_profile;

void setUp(void) {
}

void tearDown(void) {
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void read_text(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "rb");
    size_t count;

    TEST_ASSERT_NOT_NULL(file);
    count = fread(buffer, 1, size - 1, file);
    TEST_ASSERT_FALSE(ferror(file));
    buffer[count] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static CupError count_entry(const char *path, SystemPathKind kind, void *userdata) {
    size_t *count = userdata;

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_TRUE(kind != SYSTEM_PATH_MISSING);
    (*count)++;
    return CUP_OK;
}

static CupError reject_entry(const char *path, SystemPathKind kind, void *userdata) {
    (void)path;
    (void)kind;
    (void)userdata;
    return CUP_ERR_INTERRUPT;
}

static int always_cancel(void) {
    return 1;
}

static int wait_for_path(const char *path) {
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        DWORD attributes = GetFileAttributesA(path);

        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return 1;
        }
        Sleep(50);
    }
    return 0;
}

static int create_directory_junction(const char *link_path, const char *target_path) {
    char absolute_link[CUP_TEST_TEMP_PATH_SIZE];
    char absolute_target[CUP_TEST_TEMP_PATH_SIZE];
    char command[CUP_TEST_TEMP_PATH_SIZE * 3];
    DWORD link_length;
    DWORD target_length;
    size_t i;
    int written;

    link_length = GetFullPathNameA(
        link_path, (DWORD)sizeof(absolute_link), absolute_link, NULL);
    target_length = GetFullPathNameA(
        target_path, (DWORD)sizeof(absolute_target), absolute_target, NULL);
    if (link_length == 0 || link_length >= sizeof(absolute_link) ||
        target_length == 0 || target_length >= sizeof(absolute_target)) {
        return 0;
    }
    for (i = 0; absolute_link[i] != '\0'; ++i) {
        if (absolute_link[i] == '/') {
            absolute_link[i] = '\\';
        }
    }
    for (i = 0; absolute_target[i] != '\0'; ++i) {
        if (absolute_target[i] == '/') {
            absolute_target[i] = '\\';
        }
    }
    written = snprintf(command,
                       sizeof(command),
                       "cmd.exe /d /c mklink /J \"%s\" \"%s\" >NUL",
                       absolute_link,
                       absolute_target);
    return written > 0 && (size_t)written < sizeof(command) && system(command) == 0;
}

static void test_home_and_process_identity(void) {
    char buffer[CUP_TEST_TEMP_PATH_SIZE];
    char expected[CUP_TEST_TEMP_PATH_SIZE];
    char volume[CUP_TEST_TEMP_PATH_SIZE];
    DWORD length;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(buffer, 0));

    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", temp_dir));
    length = GetFullPathNameA(temp_dir, (DWORD)sizeof(expected), expected, NULL);
    TEST_ASSERT_TRUE(length > 0 && length < sizeof(expected));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(path_equal(expected, buffer));
    TEST_ASSERT_NULL(strchr(buffer, '\\'));
    TEST_ASSERT_TRUE(system_get_process_id() > 0);

    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", ""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", "relative-profile"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));

    TEST_ASSERT_TRUE(GetVolumePathNameA(expected, volume, (DWORD)sizeof(volume)));
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("USERPROFILE", volume));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));

    TEST_ASSERT_EQUAL_INT(0,
                          _putenv_s("USERPROFILE", had_profile ? original_profile : ""));
}

static void test_paths_permissions_and_traversal(void) {
    char directory[CUP_TEST_TEMP_PATH_SIZE];
    char nested[CUP_TEST_TEMP_PATH_SIZE];
    char executable[CUP_TEST_TEMP_PATH_SIZE];
    char script[CUP_TEST_TEMP_PATH_SIZE];
    char batch[CUP_TEST_TEMP_PATH_SIZE];
    SystemPathKind kind;
    long long size;
    size_t count = 0;
    int value;

    build_path(directory, sizeof(directory), "paths");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(directory, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_DIRECTORY, kind);

    TEST_ASSERT_TRUE(snprintf(executable, sizeof(executable), "%s/tool.exe", directory) > 0);
    TEST_ASSERT_TRUE(snprintf(script, sizeof(script), "%s/uninstall.ps1", directory) > 0);
    TEST_ASSERT_TRUE(snprintf(batch, sizeof(batch), "%s/wrapper.cmd", directory) > 0);
    write_text(executable, "binary");
    write_text(script, "script");
    write_text(batch, "@echo off\r\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(executable, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_file_size(executable, &size));
    TEST_ASSERT_EQUAL_INT(6, size);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(script, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(script, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(script, &value));
    TEST_ASSERT_FALSE(value);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(executable, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(executable, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(batch, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(script, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(script, &value));
    TEST_ASSERT_FALSE(value);

    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", directory) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(nested));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_list_directory(directory, count_entry, &count));
    TEST_ASSERT_EQUAL_size_t(4, count);
    count = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_walk_directory(directory, count_entry, &count));
    TEST_ASSERT_EQUAL_size_t(4, count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_list_directory(directory, reject_entry, NULL));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(NULL, &kind));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(executable, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_list_directory(directory, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_list_directory(executable, count_entry, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_is_read_only("missing", &value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only("missing", 1));
}

static void test_reparse_points_are_not_followed(void) {
    char root[CUP_TEST_TEMP_PATH_SIZE];
    char external[CUP_TEST_TEMP_PATH_SIZE];
    char sentinel[CUP_TEST_TEMP_PATH_SIZE];
    char junction[CUP_TEST_TEMP_PATH_SIZE];
    SystemPathKind kind;
    int exists;

    build_path(root, sizeof(root), "junction-tree");
    build_path(external, sizeof(external), "junction-target");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(external));
    TEST_ASSERT_TRUE(snprintf(sentinel, sizeof(sentinel), "%s/sentinel.txt", external) > 0);
    write_text(sentinel, "preserve");
    TEST_ASSERT_TRUE(snprintf(junction, sizeof(junction), "%s/external", root) > 0);
    TEST_ASSERT_TRUE(create_directory_junction(junction, external));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(junction, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_LINK, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(root, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(root, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(sentinel, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(external, NULL));
}

static void test_detached_uninstall_start(void) {
    char script[CUP_TEST_TEMP_PATH_SIZE];
    char marker[CUP_TEST_TEMP_PATH_SIZE];
    char prefixed_root[CUP_TEST_TEMP_PATH_SIZE + 8];
    char script_text[CUP_TEST_TEMP_PATH_SIZE * 3];
    char contents[CUP_TEST_TEMP_PATH_SIZE * 2];
    char expected_working_directory[CUP_TEST_TEMP_PATH_SIZE];
    DWORD temp_length;
    int written;

    build_path(script, sizeof(script), "uninstall-fixture.ps1");
    build_path(marker, sizeof(marker), "uninstall-started.txt");
    written = snprintf(script_text,
                       sizeof(script_text),
                       "param([string]$CupRoot,[string]$SelfPath,[int]$ParentPid)\r\n"
                       "[IO.File]::WriteAllText($env:CUP_TEST_UNINSTALL_MARKER, "
                       "$CupRoot + [Environment]::NewLine + "
                       "$SelfPath + [Environment]::NewLine + $ParentPid + "
                       "[Environment]::NewLine + [Environment]::CurrentDirectory)\r\n"
                       "Remove-Item -LiteralPath $SelfPath -Force\r\n");
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(script_text));
    temp_length = GetTempPathA((DWORD)sizeof(expected_working_directory),
                               expected_working_directory);
    TEST_ASSERT_TRUE(
        temp_length > 0 && temp_length < sizeof(expected_working_directory));
    write_text(script, script_text);
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("CUP_TEST_UNINSTALL_MARKER", marker));
    written = snprintf(prefixed_root, sizeof(prefixed_root), "\\\\?\\%s", temp_dir);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(prefixed_root));
    {
        size_t i;

        for (i = 4; prefixed_root[i] != '\0'; ++i) {
            if (prefixed_root[i] == '/') {
                prefixed_root[i] = '\\';
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_uninstall(NULL, script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_uninstall(temp_dir, NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_uninstall(temp_dir, script, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_start_uninstall(prefixed_root, script, 999999UL));
    TEST_ASSERT_TRUE(wait_for_path(marker));
    read_text(marker, contents, sizeof(contents));
    {
        char *root = contents;
        char *self_path;
        char *parent_pid;
        char *working_directory;
        char *separator = strstr(root, "\r\n");
        size_t separator_size = 2;

        if (separator == NULL) {
            separator = strchr(root, '\n');
            separator_size = 1;
        }
        TEST_ASSERT_NOT_NULL(separator);
        *separator = '\0';
        self_path = separator + separator_size;
        separator = strstr(self_path, "\r\n");
        separator_size = 2;
        if (separator == NULL) {
            separator = strchr(self_path, '\n');
            separator_size = 1;
        }
        TEST_ASSERT_NOT_NULL(separator);
        *separator = '\0';
        parent_pid = separator + separator_size;
        separator = strstr(parent_pid, "\r\n");
        separator_size = 2;
        if (separator == NULL) {
            separator = strchr(parent_pid, '\n');
            separator_size = 1;
        }
        TEST_ASSERT_NOT_NULL(separator);
        *separator = '\0';
        working_directory = separator + separator_size;

        TEST_ASSERT_TRUE(path_equal(root, temp_dir));
        TEST_ASSERT_FALSE(strncmp(root, "\\\\?\\", 4) == 0);
        TEST_ASSERT_NULL(strchr(root, '/'));
        TEST_ASSERT_TRUE(self_path[0] != '\0');
        TEST_ASSERT_FALSE(strncmp(self_path, "\\\\?\\", 4) == 0);
        TEST_ASSERT_NULL(strchr(self_path, '/'));
        TEST_ASSERT_EQUAL_STRING("999999", parent_pid);
        TEST_ASSERT_TRUE(path_equal(working_directory, expected_working_directory));
    }
    TEST_ASSERT_EQUAL_INT(0, _putenv_s("CUP_TEST_UNINSTALL_MARKER", ""));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(marker));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(script));
}

static void test_copy_replace_and_temporary_objects(void) {
    char source[CUP_TEST_TEMP_PATH_SIZE];
    char copy[CUP_TEST_TEMP_PATH_SIZE];
    char moved[CUP_TEST_TEMP_PATH_SIZE];
    char replacement[CUP_TEST_TEMP_PATH_SIZE];
    char exclusive[CUP_TEST_TEMP_PATH_SIZE];
    char temporary[CUP_TEST_TEMP_PATH_SIZE];
    char temporary_directory[CUP_TEST_TEMP_PATH_SIZE];
    char unique[CUP_TEST_TEMP_PATH_SIZE];
    char buffer[64];
    FILE *file = NULL;
    SystemCommitState state;
    int exists;

    build_path(source, sizeof(source), "source.exe");
    build_path(copy, sizeof(copy), "copy.exe");
    build_path(moved, sizeof(moved), "moved.exe");
    build_path(replacement, sizeof(replacement), "replacement.exe");
    build_path(exclusive, sizeof(exclusive), "exclusive.tmp");
    write_text(source, "source-data");

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, copy));
    read_text(copy, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, source));
    read_text(source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);

    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(copy, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(copy, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(moved, &exists));
    TEST_ASSERT_TRUE(exists);

    write_text(replacement, "new-data");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_replace_file(replacement, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    read_text(moved, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("new-data", buffer);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_file_exclusive(exclusive, &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_file(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, system_create_file_exclusive(exclusive, &file));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_file(
                              temp_dir, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(temporary, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_directory(temp_dir,
                                                       "directory",
                                                       temporary_directory,
                                                       sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_make_unique_temp_path(
                              temp_dir, "unique", unique, sizeof(unique)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(unique, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_parent_directory(moved));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_sync_parent_directory(""));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(exclusive));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(temporary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(temporary_directory));
}

static void test_private_directory_tree_removal_and_locks(void) {
    char private_directory[CUP_TEST_TEMP_PATH_SIZE];
    char tree[CUP_TEST_TEMP_PATH_SIZE];
    char child[CUP_TEST_TEMP_PATH_SIZE];
    char lock_path[CUP_TEST_TEMP_PATH_SIZE];
    SystemLock first = {0};
    SystemLock second = {0};
    int is_private;

    build_path(private_directory, sizeof(private_directory), "private");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_private_directory(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_directory_is_private(private_directory, &is_private));
    TEST_ASSERT_TRUE(is_private);

    build_path(tree, sizeof(tree), "tree");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(tree));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, system_remove_tree(tree, always_cancel));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/child.txt", tree) > 0);
    write_text(child, "child");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, system_remove_tree(tree, always_cancel));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(tree, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(tree, NULL));

    build_path(lock_path, sizeof(lock_path), "cup.lock");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&first, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK,
                          system_lock_acquire(&second, lock_path, SYSTEM_LOCK_SHARED));
    system_lock_release(&second);
    system_lock_release(&first);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&second, lock_path, SYSTEM_LOCK_SHARED));
    system_lock_release(&second);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(private_directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(private_directory));
}

int main(void) {
    DWORD length = GetEnvironmentVariableA(
        "USERPROFILE", original_profile, (DWORD)sizeof(original_profile));

    had_profile = length > 0 && length < sizeof(original_profile);
    if (test_make_temp_directory(
            temp_dir, sizeof(temp_dir), "cup-system-windows-test") == NULL) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_home_and_process_identity);
    RUN_TEST(test_paths_permissions_and_traversal);
    RUN_TEST(test_reparse_points_are_not_followed);
    RUN_TEST(test_detached_uninstall_start);
    RUN_TEST(test_copy_replace_and_temporary_objects);
    RUN_TEST(test_private_directory_tree_removal_and_locks);
    {
        int result = UNITY_END();

        if (had_profile) {
            (void)_putenv_s("USERPROFILE", original_profile);
        } else {
            (void)_putenv_s("USERPROFILE", "");
        }
        (void)test_remove_tree(temp_dir);
        return result;
    }
}
