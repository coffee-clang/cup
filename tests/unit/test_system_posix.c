/*
 * Test focus: Exercises POSIX path kinds, permissions, locks, exclusive temporary objects,
 * durable moves and detached process helpers.
 */

#include "constants.h"
#include "error.h"
#include "system.h"
#include "unity.h"

void setUp(void);
void tearDown(void);

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[] = "/tmp/cup-system-test-XXXXXX";
static char original_home[1024];
static int had_home;

/* Fixture lifecycle and local construction helpers. */

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

static int wait_for_path(const char *path, int should_exist) {
    struct timespec pause = {0, 20000000L};
    int attempt;

    for (attempt = 0; attempt < 250; ++attempt) {
        int exists = access(path, F_OK) == 0;
        if (exists == should_exist) {
            return 1;
        }
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}

/* Test cases grouped by the public contract they exercise. */

static void test_home_process(void) {
    char buffer[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_home_dir(buffer, 0));

    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING(temp_dir, buffer);
    TEST_ASSERT_TRUE(system_get_process_id() > 0);

    TEST_ASSERT_EQUAL_INT(0, unsetenv("HOME"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "relative", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));

    if (had_home) {
        TEST_ASSERT_EQUAL_INT(0, setenv("HOME", original_home, 1));
    } else {
        TEST_ASSERT_EQUAL_INT(0, unsetenv("HOME"));
    }
}

static CupError count_callback(const char *path, SystemPathKind kind, void *userdata) {
    size_t *count = userdata;

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_TRUE(kind != SYSTEM_PATH_MISSING);
    (*count)++;
    return CUP_OK;
}

static CupError fail_callback(const char *path, SystemPathKind kind, void *userdata) {
    (void)path;
    (void)kind;
    (void)userdata;
    return CUP_ERR_INTERRUPT;
}

static void test_path_and_walk(void) {
    char directory[1024];
    char nested[1024];
    char file_path[1024];
    char link_path[1024];
    SystemPathKind kind;
    long long size;
    int value;
    size_t count = 0;

    /* Directory and regular-file queries report stable path kinds and sizes. */
    build_path(directory, sizeof(directory), "directory");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(directory, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_DIRECTORY, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(directory, &value));
    TEST_ASSERT_TRUE(value);

    TEST_ASSERT_TRUE(snprintf(file_path, sizeof(file_path), "%s/file", directory) > 0);
    write_text(file_path, "hello");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(file_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_regular_file(file_path, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_file_size(file_path, &size));
    TEST_ASSERT_EQUAL_INT(5, size);

    /* Read-only and executable attributes can be toggled and observed. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(file_path, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(file_path, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_read_only(file_path, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_read_only(file_path, 0));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(file_path, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(file_path, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(file_path, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(file_path, &value));
    TEST_ASSERT_FALSE(value);

    /* Non-recursive listing and recursive walking classify links without following them. */
    TEST_ASSERT_TRUE(snprintf(nested, sizeof(nested), "%s/nested", directory) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(nested));
    TEST_ASSERT_TRUE(snprintf(link_path, sizeof(link_path), "%s/link", directory) > 0);
    TEST_ASSERT_EQUAL_INT(0, symlink("file", link_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(link_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_LINK, kind);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_list_directory(directory, count_callback, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    count = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_walk_directory(directory, count_callback, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, system_list_directory(directory, fail_callback, NULL));

    /* Public argument and path-type errors remain distinct from successful empty traversal. */
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(NULL, &kind));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_get_path_kind(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_list_directory(directory, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_list_directory(file_path, count_callback, &count));
}

static void test_copy_move_temp(void) {
    char source[1024];
    char copy[1024];
    char moved[1024];
    char replacement[1024];
    char exclusive[1024];
    char temporary[1024];
    char temporary_directory[1024];
    char unique[1024];
    char buffer[64];
    FILE *file = NULL;
    SystemCommitState state;
    int exists;

    build_path(source, sizeof(source), "source");
    build_path(copy, sizeof(copy), "copy");
    build_path(moved, sizeof(moved), "moved");
    build_path(replacement, sizeof(replacement), "replacement");
    build_path(exclusive, sizeof(exclusive), "exclusive");
    write_text(source, "source-data");

    /* Copies preserve bytes and executable permissions, including self-copy. */
    TEST_ASSERT_EQUAL_INT(0, chmod(source, 0750));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, copy));
    {
        struct stat copy_info;
        TEST_ASSERT_EQUAL_INT(0, stat(copy, &copy_info));
        TEST_ASSERT_EQUAL_INT(0750, copy_info.st_mode & 0777);
    }
    file = fopen(copy, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(11, fread(buffer, 1, sizeof(buffer), file));
    TEST_ASSERT_FALSE(ferror(file));
    buffer[11] = '\0';
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file(source, source));
    file = fopen(source, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(11, fread(buffer, 1, sizeof(buffer), file));
    TEST_ASSERT_FALSE(ferror(file));
    buffer[11] = '\0';
    TEST_ASSERT_EQUAL_STRING("source-data", buffer);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;

    /* Move and replace operations report the durable commit boundary. */
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(copy, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(copy, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(moved, &exists));
    TEST_ASSERT_TRUE(exists);

    write_text(replacement, "new");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_replace_file(replacement, moved, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);

    /* Exclusive and temporary creation return caller-owned handles and unique paths. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_file_exclusive(exclusive, &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("exclusive", file) >= 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_file(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, system_create_file_exclusive(exclusive, &file));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, system_create_temp_file(temp_dir, "file", temporary, sizeof(temporary), &file));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(temporary, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_create_temp_directory(
                              temp_dir, "dir", temporary_directory, sizeof(temporary_directory)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(temporary_directory, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_make_unique_temp_path(temp_dir, "unique", unique, sizeof(unique)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(unique, &exists));
    TEST_ASSERT_FALSE(exists);

    /* Removal is idempotent for files but rejects non-empty directories. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_parent_directory(moved));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(moved));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(moved));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_directory(temp_dir));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_copy_file(NULL, copy));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_move_path(source, moved, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_sync_file(NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_directory(NULL, "x", temporary_directory, sizeof(temporary_directory)));
}

static void test_lock_contention(void) {
    char lock_path[1024];
    SystemLock lock = {0, 0};
    pid_t child;
    int status;

    build_path(lock_path, sizeof(lock_path), "cup.lock");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_TRUE(lock.active);

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        SystemLock child_lock = {0, 0};
        CupError err = system_lock_acquire(&child_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) {
            system_lock_release(&child_lock);
        }
        _exit(err == CUP_ERR_LOCK ? 0 : 1);
    }

    TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    system_lock_release(&lock);
    TEST_ASSERT_FALSE(lock.active);
    system_lock_release(&lock);
    system_lock_release(NULL);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_acquire(NULL, lock_path, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_acquire(&lock, lock_path, (SystemLockMode)99));
}

static void assert_directory_contracts(const char *file_path,
                                       const char *directory,
                                       const char *missing) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_make_directory(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_make_directory(file_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_directory(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_directory(file_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(directory));
}

static void assert_move_contracts(const char *file_path,
                                  const char *missing,
                                  const char *destination) {
    SystemCommitState state;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_move_path(NULL, destination, &state));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_move_path(file_path, NULL, &state));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_move_path(file_path, destination, NULL));
    write_text(destination, "occupied");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_move_path(file_path, destination, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(destination));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_move_path(missing, destination, &state));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_replace_file(NULL, destination, &state));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_replace_file(file_path, NULL, &state));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_replace_file(file_path, destination, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_replace_file(missing, destination, &state));
}

static void assert_copy_remove_contracts(const char *file_path,
                                         const char *directory,
                                         const char *missing,
                                         const char *destination,
                                         const char *link_path) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_file(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_file(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(link_path));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_copy_file(missing, destination));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_copy_file(file_path, "/missing-parent/file"));
    TEST_ASSERT_EQUAL_INT(0, symlink("contracts-file", link_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_copy_file(link_path, destination));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(link_path));
}

static void assert_temp_contracts(const char *file_path, char *destination, char *tiny) {
    FILE *file = NULL;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_sync_parent_directory(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_create_file_exclusive(NULL, &file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_create_file_exclusive(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_create_file_exclusive("/missing-parent/file", &file));

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_file(NULL, "x", destination, 1024, &file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_file(temp_dir, NULL, destination, 1024, &file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_file(temp_dir, "x", NULL, 1024, &file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_file(temp_dir, "x", destination, 0, &file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_file(temp_dir, "long-prefix", tiny, 2, &file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        system_create_temp_file("/missing-parent", "x", destination, 1024, &file));

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_directory(temp_dir, NULL, destination, 1024));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_directory(temp_dir, "x", NULL, 1024));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_directory(temp_dir, "x", destination, 0));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        system_create_temp_directory("/missing-parent", "x", destination, 1024));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_make_unique_temp_path(NULL, "x", destination, 1024));
}

static void assert_path_query_contracts(const char *file_path,
                                        const char *directory,
                                        const char *missing,
                                        const char *link_path) {
    long long size;
    int value;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_path_exists(file_path, NULL));
    value = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_path_exists(NULL, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(missing, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_directory(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_regular_file(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_regular_file(directory, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_file_size(NULL, &size));
    TEST_ASSERT_EQUAL_INT(0, size);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_file_size(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_file_size(directory, &size));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_executable(NULL, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_executable(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_read_only(NULL, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_is_read_only(file_path, NULL));
    TEST_ASSERT_EQUAL_INT(0, symlink("contracts-file", link_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_is_read_only(link_path, &value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only(link_path, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(link_path, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(link_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_read_only(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_executable(NULL, 1));
}

static void assert_walk_lock_contracts(const char *file_path,
                                       const char *directory,
                                       const char *missing) {
    SystemLock lock = {0, 0};
    size_t count = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_list_directory(missing, count_callback, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_walk_directory(directory, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_walk_directory(NULL, count_callback, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_walk_directory(file_path, count_callback, &count));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_acquire(&lock, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire(&lock, directory, SYSTEM_LOCK_SHARED));
}

static void test_api_errors(void) {
    char file_path[1024];
    char directory[1024];
    char missing[1024];
    char destination[1024];
    char link_path[1024];
    char tiny[2];
    SystemPathKind kind;

    build_path(file_path, sizeof(file_path), "contracts-file");
    build_path(directory, sizeof(directory), "contracts-directory");
    build_path(missing, sizeof(missing), "contracts-missing");
    build_path(destination, sizeof(destination), "contracts-destination");
    build_path(link_path, sizeof(link_path), "contracts-link");
    write_text(file_path, "data");
    TEST_ASSERT_EQUAL_INT(0, symlink("contracts-file", link_path));

    assert_directory_contracts(file_path, directory, missing);
    assert_move_contracts(file_path, missing, destination);
    assert_copy_remove_contracts(file_path, directory, missing, destination, link_path);
    assert_temp_contracts(file_path, destination, tiny);
    assert_path_query_contracts(file_path, directory, missing, link_path);
    assert_walk_lock_contracts(file_path, directory, missing);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(file_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(directory));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(missing, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);
}

static void test_extra_paths(void) {
    char first_dir[1024];
    char second_dir[1024];
    char source[1024];
    char destination[1024];
    char fifo_path[1024];
    char home_buffer[2];
    SystemCommitState state;
    SystemPathKind kind;
    size_t count = 0;
    char cwd[1024];

    build_path(first_dir, sizeof(first_dir), "move-source-dir");
    build_path(second_dir, sizeof(second_dir), "move-target-dir");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(first_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(second_dir));
    TEST_ASSERT_TRUE(snprintf(source, sizeof(source), "%s/item", first_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(destination, sizeof(destination), "%s/item", second_dir) > 0);
    write_text(source, "cross-directory");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(source, destination, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);

    build_path(fifo_path, sizeof(fifo_path), "named-pipe");
    TEST_ASSERT_EQUAL_INT(0, mkfifo(fifo_path, 0600));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(fifo_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_OTHER, kind);

    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    TEST_ASSERT_EQUAL_INT(0, chdir(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_parent_directory("relative-file"));
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));

    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          system_get_home_dir(home_buffer, sizeof(home_buffer)));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_walk_directory(second_dir, fail_callback, &count));
}

static void test_uninstall_helper(void) {
    char script[1024];
    char marker[1024];
    char contents[4096];
    char script_text[4096];

    build_path(script, sizeof(script), "uninstall-helper.sh");
    build_path(marker, sizeof(marker), "uninstall-helper.out");
    TEST_ASSERT_TRUE(snprintf(script_text,
                              sizeof(script_text),
                              "#!/bin/sh\n"
                              "printf '%%s\\n%%s\\n%%s\\n' \"$1\" \"$2\" \"$3\" > '%s'\n"
                              "rm -f \"$2\"\n",
                              marker) > 0);
    write_text(script, script_text);
    TEST_ASSERT_EQUAL_INT(0, chmod(script, 0755));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_start_uninstall(NULL, script, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_start_uninstall(temp_dir, NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_start_uninstall(temp_dir, script, 0));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_start_uninstall(temp_dir, script, 999999UL));
    TEST_ASSERT_TRUE(wait_for_path(marker, 1));
    read_text(marker, contents, sizeof(contents));
    TEST_ASSERT_NOT_NULL(strstr(contents, temp_dir));
    TEST_ASSERT_NOT_NULL(strstr(contents, "999999"));
}

/* Suite registration. */

void register_system_posix_tests(void) {
    const char *home = getenv("HOME");

    if (home != NULL) {
        had_home = 1;
        TEST_ASSERT_TRUE(snprintf(original_home, sizeof(original_home), "%s", home) > 0);
    }
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));

    RUN_TEST(test_home_process);
    RUN_TEST(test_path_and_walk);
    RUN_TEST(test_copy_move_temp);
    RUN_TEST(test_lock_contention);
    RUN_TEST(test_api_errors);
    RUN_TEST(test_extra_paths);
    RUN_TEST(test_uninstall_helper);
}
