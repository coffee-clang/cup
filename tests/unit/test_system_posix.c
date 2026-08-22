/*
 * Exercises POSIX path kinds, permissions, locks, exclusive temporary objects,
 * durable moves and detached process helpers.
 */

#include "constants.h"
#include "error.h"
#include "system.h"
#include "test_platform.h"
#include "unity.h"

void setUp(void);
void tearDown(void);

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
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
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/tmp/cup//home", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/tmp/cup/./home", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/tmp/cup/../home", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/tmp/cup/home/", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", "/tmp/cup\\home", 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_get_home_dir(buffer, sizeof(buffer)));

    if (had_home) {
        TEST_ASSERT_EQUAL_INT(0, setenv("HOME", original_home, 1));
    } else {
        TEST_ASSERT_EQUAL_INT(0, unsetenv("HOME"));
    }
}

static CupError count_callback(const char *path,
                               SystemPathKind kind,
                               const SystemPathIdentity *identity,
                               void *userdata) {
    size_t *count = userdata;

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_TRUE(identity->valid);
    TEST_ASSERT_EQUAL_INT(kind, identity->kind);
    TEST_ASSERT_TRUE(kind != SYSTEM_PATH_MISSING);
    (*count)++;
    return CUP_OK;
}

static CupError fail_callback(const char *path,
                              SystemPathKind kind,
                              const SystemPathIdentity *identity,
                              void *userdata) {
    (void)identity;
    (void)path;
    (void)kind;
    (void)userdata;
    return CUP_ERR_INTERRUPT;
}

static int cancellation_requested(void) {
    return 1;
}

static unsigned int cancellation_checks_remaining;

static int cancellation_after_checks(void) {
    if (cancellation_checks_remaining == 0) {
        return 1;
    }
    cancellation_checks_remaining--;
    return 0;
}

static void test_path_and_walk(void) {
    char directory[1024];
    char nested[1024];
    char file_path[1024];
    char original_path[1024];
    char link_path[1024];
    SystemPathKind kind;
    SystemPathIdentity first_identity;
    SystemPathIdentity second_identity;
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
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(file_path, &first_identity));
    TEST_ASSERT_TRUE(first_identity.valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, first_identity.kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(file_path, &second_identity));
    TEST_ASSERT_TRUE(system_path_identity_equal(&first_identity, &second_identity));
    TEST_ASSERT_TRUE(snprintf(original_path, sizeof(original_path), "%s/original", directory) > 0);
    TEST_ASSERT_EQUAL_INT(0, rename(file_path, original_path));
    write_text(file_path, "hello");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(file_path, &second_identity));
    TEST_ASSERT_FALSE(system_path_identity_equal(&first_identity, &second_identity));
    TEST_ASSERT_EQUAL_INT(0, unlink(original_path));

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
    char identity_target[1024];
    char identity_original[1024];
    char identity_source[1024];
    char exclusive[1024];
    char temporary[1024];
    char temporary_directory[1024];
    char unique[1024];
    char buffer[64];
    FILE *file = NULL;
    SystemCommitState state;
    SystemPathIdentity expected_identity;
    int exists;

    build_path(source, sizeof(source), "source");
    build_path(copy, sizeof(copy), "copy");
    build_path(moved, sizeof(moved), "moved");
    build_path(replacement, sizeof(replacement), "replacement");
    build_path(identity_target, sizeof(identity_target), "identity-target");
    build_path(identity_original, sizeof(identity_original), "identity-original");
    build_path(identity_source, sizeof(identity_source), "identity-source");
    build_path(exclusive, sizeof(exclusive), "script-exclusive");
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

    /* Identity-bound mutation preserves a replacement that appeared after the snapshot. */
    write_text(identity_target, "original");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_target, &expected_identity));
    TEST_ASSERT_EQUAL_INT(0, rename(identity_target, identity_original));

    write_text(identity_source, "new-value");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_replace_file_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_target, &exists));
    TEST_ASSERT_FALSE(exists);
    read_text(identity_source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("new-value", buffer);

    write_text(identity_target, "foreign");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_remove_file_if_identity(identity_target, &expected_identity));
    read_text(identity_target, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("foreign", buffer);

    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_replace_file_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    read_text(identity_target, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("foreign", buffer);
    read_text(identity_source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("new-value", buffer);
    {
        SystemPathIdentity wrong_kind = expected_identity;

        wrong_kind.kind = SYSTEM_PATH_LINK;
        state = SYSTEM_COMMIT_NOT_APPLIED;
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            system_replace_file_if_identity(
                identity_source, identity_target, &wrong_kind, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT, system_remove_file_if_identity(identity_target, &wrong_kind));
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_source));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_target));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_original));

    /* Source-identity-bound moves reject a pathname that no longer names the observed object. */
    write_text(identity_source, "move-old");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(identity_source, identity_original, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    write_text(identity_source, "move-foreign");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    read_text(identity_source, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("move-foreign", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_target, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    {
        SystemPathIdentity wrong_kind = expected_identity;

        wrong_kind.kind = SYSTEM_PATH_LINK;
        state = SYSTEM_COMMIT_NOT_APPLIED;
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            system_move_path_if_identity(
                identity_source, identity_target, &wrong_kind, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    }
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    read_text(identity_target, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("move-foreign", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_target));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(identity_original));

    /* Directory sources use the same source-identity contract. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(identity_source));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_get_path_identity(identity_source, &expected_identity));
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        system_move_path_if_identity(
            identity_source, identity_target, &expected_identity, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(identity_source, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(identity_target));

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
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_file_executable(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_file_executable(file, 2));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_file_executable(file, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_file(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file = NULL;
    {
        struct stat temporary_info;
        TEST_ASSERT_EQUAL_INT(0, stat(temporary, &temporary_info));
        TEST_ASSERT_EQUAL_INT(0700, temporary_info.st_mode & 0777);
    }
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
    SystemLock lock = {0};
    pid_t child;
    int status;
    int exists;

    build_path(lock_path, sizeof(lock_path), "cup.lock");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_TRUE(lock.active);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_TRUE(lock.active);

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        SystemLock child_lock = {0};
        CupError err = system_lock_acquire(&child_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) {
            system_lock_release(&child_lock);
        }
        _exit(err == CUP_ERR_LOCK ? 0 : 1);
    }

    TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    /* Closing an independently opened descriptor for cup.lock must not release
     * the lock held by SystemLock. */
    {
        int extra_fd = open(lock_path, O_RDONLY | O_CLOEXEC);

        TEST_ASSERT_TRUE(extra_fd >= 0);
        TEST_ASSERT_EQUAL_INT(0, close(extra_fd));
    }
    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        SystemLock child_lock = {0};
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

    {
        SystemLock first_shared = {0};
        SystemLock second_shared = {0};

        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              system_lock_acquire(&first_shared, lock_path, SYSTEM_LOCK_SHARED));
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              system_lock_acquire(&second_shared, lock_path, SYSTEM_LOCK_SHARED));
        system_lock_release(&first_shared);

        child = fork();
        TEST_ASSERT_TRUE(child >= 0);
        if (child == 0) {
            SystemLock child_lock = {0};
            CupError err = system_lock_acquire(&child_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
            if (err == CUP_OK) {
                system_lock_release(&child_lock);
            }
            _exit(err == CUP_ERR_LOCK ? 0 : 1);
        }
        TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
        TEST_ASSERT_TRUE(WIFEXITED(status));
        TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
        system_lock_release(&second_shared);
    }

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(lock_path, &exists));
    TEST_ASSERT_FALSE(exists);
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
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_file(temp_dir, "../escape", destination, 1024, &file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_file(temp_dir, "x", NULL, 1024, &file));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_file(temp_dir, "x", destination, 0, &file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        system_create_temp_file(temp_dir, "long-prefix", tiny, 2, &file));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TEMPORARY,
        system_create_temp_file("/missing-parent", "x", destination, 1024, &file));

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_directory(temp_dir, NULL, destination, 1024));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        system_create_temp_directory(temp_dir, "../escape", destination, 1024));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_directory(temp_dir, "x", NULL, 1024));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_create_temp_directory(temp_dir, "x", destination, 0));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        system_create_temp_directory(temp_dir, "long-prefix", tiny, 2));
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
    value = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(missing, &value));
    TEST_ASSERT_FALSE(value);
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
    SystemLock lock = {0};
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
    char overlong[MAX_PATH_LEN + 1];
    SystemLock overlong_lock = {0};
    size_t count = 0;
    SystemPathKind kind;

    memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';

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

    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, system_make_directory(overlong));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, system_sync_parent_directory(overlong));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          system_list_directory(overlong, count_callback, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          system_walk_directory(overlong, count_callback, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          system_lock_acquire(&overlong_lock,
                                              overlong,
                                              SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_FALSE(overlong_lock.active);

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
    int read_only = 0;
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
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_is_read_only(fifo_path, &read_only));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only(fifo_path, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_read_only(destination, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_set_executable(destination, -1));

    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    TEST_ASSERT_EQUAL_INT(0, chdir(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_sync_parent_directory("relative-file"));
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));

    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          system_get_home_dir(home_buffer, sizeof(home_buffer)));
    if (had_home) {
        TEST_ASSERT_EQUAL_INT(0, setenv("HOME", original_home, 1));
    } else {
        TEST_ASSERT_EQUAL_INT(0, unsetenv("HOME"));
    }

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_walk_directory(second_dir, fail_callback, &count));

    /* Parent-directory authority, not file-data readability, governs rename and unlink. */
    {
        char unreadable[1024];
        char moved_unreadable[1024];
        char unreadable_tree[1024];
        char unreadable_child[1024];

        build_path(unreadable, sizeof(unreadable), "unreadable-file");
        build_path(moved_unreadable, sizeof(moved_unreadable), "unreadable-moved");
        write_text(unreadable, "data");
        TEST_ASSERT_EQUAL_INT(0, chmod(unreadable, 0000));
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_move_path(unreadable, moved_unreadable, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(moved_unreadable));
        TEST_ASSERT_NOT_EQUAL(0, access(moved_unreadable, F_OK));

        {
            char unreadable_directory[1024];

            build_path(unreadable_directory,
                       sizeof(unreadable_directory),
                       "unreadable-empty-directory");
            TEST_ASSERT_EQUAL_INT(0, mkdir(unreadable_directory, 0700));
            TEST_ASSERT_EQUAL_INT(0, chmod(unreadable_directory, 0000));
            TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_directory(unreadable_directory));
            TEST_ASSERT_NOT_EQUAL(0, access(unreadable_directory, F_OK));
        }

        build_path(unreadable_tree, sizeof(unreadable_tree), "unreadable-tree");
        TEST_ASSERT_EQUAL_INT(0, mkdir(unreadable_tree, 0700));
        TEST_ASSERT_TRUE(snprintf(unreadable_child, sizeof(unreadable_child), "%s/file", unreadable_tree) > 0);
        write_text(unreadable_child, "data");
        TEST_ASSERT_EQUAL_INT(0, chmod(unreadable_child, 0000));
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(unreadable_tree, NULL));
        TEST_ASSERT_NOT_EQUAL(0, access(unreadable_tree, F_OK));
    }
}

static void test_trusted_operations_reject_symlinked_parent(void) {
    char real_parent[1024];
    char linked_parent[1024];
    char real_file[1024];
    char linked_file[1024];
    struct stat before;
    struct stat after;

    build_path(real_parent, sizeof(real_parent), "permission-real-parent");
    build_path(linked_parent, sizeof(linked_parent), "permission-linked-parent");
    TEST_ASSERT_EQUAL_INT(0, mkdir(real_parent, 0755));
    TEST_ASSERT_TRUE(snprintf(real_file, sizeof(real_file), "%s/value", real_parent) > 0);
    write_text(real_file, "data");
    TEST_ASSERT_EQUAL_INT(0, chmod(real_file, 0644));
    TEST_ASSERT_EQUAL_INT(0, symlink(real_parent, linked_parent));
    TEST_ASSERT_TRUE(snprintf(linked_file, sizeof(linked_file), "%s/value", linked_parent) > 0);
    TEST_ASSERT_EQUAL_INT(0, stat(real_file, &before));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_read_only(linked_file, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_set_executable(linked_file, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_sync_parent_directory(linked_file));

    {
        char linked_directory[1024];
        char linked_exclusive[1024];
        char linked_copy[1024];
        char linked_temp[1024];
        char linked_temp_directory[1024];
        char linked_unique[1024];
        char safe_copy[1024];
        FILE *exclusive_file = NULL;
        FILE *temporary_file = NULL;
        SystemLock lock = {0};

        TEST_ASSERT_TRUE(
            snprintf(linked_directory, sizeof(linked_directory), "%s/new-directory", linked_parent) > 0);
        TEST_ASSERT_TRUE(
            snprintf(linked_exclusive, sizeof(linked_exclusive), "%s/new-file", linked_parent) > 0);
        TEST_ASSERT_TRUE(snprintf(linked_copy, sizeof(linked_copy), "%s/value", linked_parent) > 0);
        build_path(safe_copy, sizeof(safe_copy), "symlink-parent-copy");

        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_make_directory(linked_directory));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_create_file_exclusive(linked_exclusive, &exclusive_file));
        TEST_ASSERT_NULL(exclusive_file);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_copy_file(linked_copy, safe_copy));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_FILESYSTEM,
            system_create_temp_file(
                linked_parent, "temp", linked_temp, sizeof(linked_temp), &temporary_file));
        TEST_ASSERT_NULL(temporary_file);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_FILESYSTEM,
            system_create_temp_directory(
                linked_parent, "temp-dir", linked_temp_directory, sizeof(linked_temp_directory)));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_FILESYSTEM,
            system_make_unique_temp_path(
                linked_parent, "unique", linked_unique, sizeof(linked_unique)));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_lock_acquire(&lock, linked_exclusive, SYSTEM_LOCK_EXCLUSIVE));
        TEST_ASSERT_FALSE(lock.active);
        {
            size_t count = 0;

            TEST_ASSERT_EQUAL_INT(
                CUP_ERR_FILESYSTEM, system_list_directory(linked_directory, count_callback, &count));
            TEST_ASSERT_EQUAL_INT(
                CUP_ERR_FILESYSTEM, system_walk_directory(linked_directory, count_callback, &count));
            TEST_ASSERT_EQUAL_size_t(0, count);
        }
        TEST_ASSERT_TRUE(access(safe_copy, F_OK) != 0);
        TEST_ASSERT_TRUE(access(linked_temp, F_OK) != 0);
        TEST_ASSERT_TRUE(access(linked_temp_directory, F_OK) != 0);
        TEST_ASSERT_TRUE(access(linked_unique, F_OK) != 0);
    }

    {
        char real_private[1024];
        char linked_private[1024];
        int is_private = 0;

        TEST_ASSERT_TRUE(snprintf(real_private, sizeof(real_private), "%s/private", real_parent) > 0);
        TEST_ASSERT_TRUE(
            snprintf(linked_private, sizeof(linked_private), "%s/private", linked_parent) > 0);
        TEST_ASSERT_EQUAL_INT(0, mkdir(real_private, 0755));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_directory_is_private(linked_private, &is_private));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_make_private_directory(linked_private));
        TEST_ASSERT_EQUAL_INT(0, stat(real_private, &after));
        TEST_ASSERT_EQUAL_INT(0755, after.st_mode & 0777);
    }

    TEST_ASSERT_EQUAL_INT(0, stat(real_file, &after));
    TEST_ASSERT_EQUAL_INT(before.st_mode & 0777, after.st_mode & 0777);
}

static void test_private_directory_contract(void) {
    char private_dir[1024];
    char regular_file[1024];
    char missing[1024];
    char link_path[1024];
    struct stat info;
    int is_private = 1;

    build_path(private_dir, sizeof(private_dir), "private-directory");
    build_path(regular_file, sizeof(regular_file), "private-file");
    build_path(missing, sizeof(missing), "private-missing");
    build_path(link_path, sizeof(link_path), "private-link");
    write_text(regular_file, "data");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_directory_is_private(NULL, &is_private));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_directory_is_private(private_dir, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(missing, &is_private));
    TEST_ASSERT_FALSE(is_private);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(regular_file, &is_private));
    TEST_ASSERT_FALSE(is_private);

    {
        SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;
        char exclusive_private[1024];

        build_path(exclusive_private, sizeof(exclusive_private), "exclusive-private");
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              system_create_private_directory(NULL, &state));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              system_create_private_directory(exclusive_private, NULL));
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              system_create_private_directory(exclusive_private, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              system_directory_is_private(exclusive_private, &is_private));
        TEST_ASSERT_TRUE(is_private);
        state = SYSTEM_COMMIT_NOT_APPLIED;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                              system_create_private_directory(exclusive_private, &state));
        TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);
    }

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_make_private_directory(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_make_private_directory(regular_file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(private_dir));
    TEST_ASSERT_EQUAL_INT(0, chmod(private_dir, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(private_dir, &is_private));
    TEST_ASSERT_FALSE(is_private);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_private_directory(private_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(private_dir, &is_private));
    TEST_ASSERT_TRUE(is_private);
    TEST_ASSERT_EQUAL_INT(0, stat(private_dir, &info));
    TEST_ASSERT_EQUAL_INT(0700, info.st_mode & 0777);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_private_directory(private_dir));

    TEST_ASSERT_EQUAL_INT(0, symlink("private-directory", link_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(link_path, &is_private));
    TEST_ASSERT_FALSE(is_private);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_make_private_directory(link_path));
}

static void test_remove_tree_path_forms(void) {
    char cwd[1024];
    char absolute[1024];
    char trailing[1024];
    char child[1024];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_tree(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_tree("", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_tree(".", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_remove_tree("..", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_remove_tree("/cup-definitely-missing-tree", NULL));

    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    TEST_ASSERT_EQUAL_INT(0, chdir(temp_dir));
    TEST_ASSERT_EQUAL_INT(0, mkdir("relative-tree", 0700));
    write_text("relative-tree/file", "data");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree("relative-tree", NULL));
    TEST_ASSERT_FALSE(access("relative-tree", F_OK) == 0);
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));

    build_path(absolute, sizeof(absolute), "trailing-tree");
    TEST_ASSERT_EQUAL_INT(0, mkdir(absolute, 0700));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/file", absolute) > 0);
    write_text(child, "data");
    TEST_ASSERT_TRUE(snprintf(trailing, sizeof(trailing), "%s///", absolute) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(trailing, NULL));
    TEST_ASSERT_FALSE(access(absolute, F_OK) == 0);

    build_path(absolute, sizeof(absolute), "cancelled-tree");
    TEST_ASSERT_EQUAL_INT(0, mkdir(absolute, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_remove_tree(absolute, cancellation_requested));
    TEST_ASSERT_TRUE(access(absolute, F_OK) == 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(absolute, NULL));

    /* Cancellation is checked again immediately before each unlink/rmdir mutation, not only
     * when recursive processing first enters an object. */
    build_path(absolute, sizeof(absolute), "cancel-before-file-unlink");
    write_text(absolute, "data");
    cancellation_checks_remaining = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_remove_tree(absolute, cancellation_after_checks));
    TEST_ASSERT_EQUAL_INT(0, access(absolute, F_OK));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(absolute));

    build_path(absolute, sizeof(absolute), "cancel-before-directory-rmdir");
    TEST_ASSERT_EQUAL_INT(0, mkdir(absolute, 0700));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/file", absolute) > 0);
    write_text(child, "data");
    cancellation_checks_remaining = 4;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_remove_tree(absolute, cancellation_after_checks));
    TEST_ASSERT_NOT_EQUAL(0, access(child, F_OK));
    TEST_ASSERT_EQUAL_INT(0, access(absolute, F_OK));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(absolute, NULL));
}

static void test_tree_depth_limit(void) {
    char root[1024];
    char current[1024];
    size_t count = 0;
    unsigned int depth;

    build_path(root, sizeof(root), "deep-tree");
    TEST_ASSERT_EQUAL_INT(0, mkdir(root, 0700));
    TEST_ASSERT_TRUE(snprintf(current, sizeof(current), "%s", root) > 0);

    for (depth = 0; depth < 130u; ++depth) {
        size_t length = strlen(current);

        TEST_ASSERT_TRUE(length + 2 < sizeof(current));
        current[length] = '/';
        current[length + 1] = 'd';
        current[length + 2] = '\0';
        TEST_ASSERT_EQUAL_INT(0, mkdir(current, 0700));
    }

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_walk_directory(root, count_callback, &count));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_remove_tree(root, NULL));

    for (depth = 0; depth < 130u; ++depth) {
        char *slash;

        TEST_ASSERT_EQUAL_INT(0, rmdir(current));
        slash = strrchr(current, '/');
        TEST_ASSERT_NOT_NULL(slash);
        *slash = '\0';
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
}

static void test_identity_bound_path_removal(void) {
    char target[1024];
    char original[1024];
    char child[1024];
    char link_path[1024];
    char external[1024];
    char sentinel[1024];
    SystemPathIdentity identity;
    SystemPathIdentity invalid = {0};

    build_path(target, sizeof(target), "identity-tree");
    build_path(original, sizeof(original), "identity-tree-original");
    build_path(link_path, sizeof(link_path), "identity-link");
    build_path(external, sizeof(external), "identity-external");

    TEST_ASSERT_EQUAL_INT(0, mkdir(target, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(target, &identity));
    TEST_ASSERT_EQUAL_INT(0, rename(target, original));
    TEST_ASSERT_EQUAL_INT(0, mkdir(target, 0700));
    TEST_ASSERT_TRUE(snprintf(child, sizeof(child), "%s/child", target) > 0);
    write_text(child, "foreign");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, system_remove_path_if_identity(target, &identity, NULL));
    TEST_ASSERT_EQUAL_INT(0, access(child, F_OK));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, system_remove_path_if_identity(target, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, system_remove_path_if_identity(target, &invalid, NULL));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(target, &identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INTERRUPT,
        system_remove_path_if_identity(target, &identity, cancellation_requested));
    TEST_ASSERT_EQUAL_INT(0, access(target, F_OK));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_path_if_identity(target, &identity, NULL));
    TEST_ASSERT_NOT_EQUAL(0, access(target, F_OK));

    TEST_ASSERT_EQUAL_INT(0, mkdir(external, 0700));
    TEST_ASSERT_TRUE(snprintf(sentinel, sizeof(sentinel), "%s/sentinel", external) > 0);
    write_text(sentinel, "preserve");
    TEST_ASSERT_EQUAL_INT(0, symlink(external, link_path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_identity(link_path, &identity));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_LINK, identity.kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_path_if_identity(link_path, &identity, NULL));
    TEST_ASSERT_NOT_EQUAL(0, access(link_path, F_OK));
    TEST_ASSERT_EQUAL_INT(0, access(sentinel, F_OK));

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(original, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(external, NULL));
}

static void test_shared_script_primitives(void) {
    char chain[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    char exclusive[MAX_PATH_LEN];
    char contents[MAX_PATH_LEN];
    char keep[MAX_PATH_LEN];
    char remove[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char buffer[32];
    SystemCommitState state;
    SystemPathIdentity identity;
    SystemLock lock = {0};
    SystemPathKind kind;
    size_t size;

    build_path(parent, sizeof(parent), "chain");
    TEST_ASSERT_TRUE(snprintf(chain, sizeof(chain), "%s/one/two", parent) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_check_directory_chain(chain, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, system_check_directory_chain(chain, 0));
    {
        char unsafe_missing[MAX_PATH_LEN];
        FILE *file = NULL;
        SystemPathIdentity file_identity;
        uint64_t file_size = 0;
        int missing = 0;

        TEST_ASSERT_TRUE(
            snprintf(unsafe_missing, sizeof(unsafe_missing), "%s/missing/../unsafe", parent) > 0);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                              system_check_directory_chain(unsafe_missing, 1));
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_INVALID_INPUT,
            system_open_regular_file(
                unsafe_missing, &file, &file_identity, &file_size, &missing));
        TEST_ASSERT_NULL(file);
        TEST_ASSERT_FALSE(file_identity.valid);
        TEST_ASSERT_EQUAL_UINT64(0, file_size);
        TEST_ASSERT_FALSE(missing);
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory_chain(chain));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_check_directory_chain(chain, 0));

    build_path(exclusive, sizeof(exclusive), "exclusive");
    state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_create_directory_exclusive(exclusive, 0700, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_DURABLE, state);
    state = SYSTEM_COMMIT_DURABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK,
                          system_create_directory_exclusive(exclusive, 0700, &state));
    TEST_ASSERT_EQUAL_INT(SYSTEM_COMMIT_NOT_APPLIED, state);

    build_path(contents, sizeof(contents), "contents");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(contents));
    TEST_ASSERT_TRUE(snprintf(keep, sizeof(keep), "%s/keep", contents) > 0);
    TEST_ASSERT_TRUE(snprintf(remove, sizeof(remove), "%s/remove", contents) > 0);
    write_text(keep, "keep");
    write_text(remove, "remove");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, system_remove_tree_contents(contents, "keep", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(keep, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(remove, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          system_remove_tree_contents(contents, NULL, cancellation_requested));

    build_path(lock_path, sizeof(lock_path), "existing.lock");
    write_text(lock_path, "format=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire_existing(
                              &lock, lock_path, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_get_identity(&lock, &identity));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, identity.kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_lock_read(&lock, buffer, sizeof(buffer), &size));
    TEST_ASSERT_EQUAL_size_t(strlen("format=1\n"), size);
    buffer[size] = '\0';
    TEST_ASSERT_EQUAL_STRING("format=1\n", buffer);
    system_lock_release(&lock);

    memset(&identity, 0xff, sizeof(identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_lock_get_identity(&lock, &identity));
    TEST_ASSERT_FALSE(identity.valid);
    size = 99;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_lock_read(&lock, buffer, sizeof(buffer), &size));
    TEST_ASSERT_EQUAL_size_t(0, size);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_lock_acquire_existing(
                              &lock, "/cup-missing-existing-lock", SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_remove_tree_contents(contents, "../unsafe", NULL));
}

static void test_handoff_primitives(void) {
    char lock_path[1024];
    char ordinary_path[1024];
    char parent_signal_value[32];
    char authority_value[32];
    SystemLock lock = {0};
    SystemLock resumed = {0};
    SystemLock competing = {0};
    SystemHandoff handoff = {0};
    int signal_fds[2];
    int authority_fd;
    int original_fd;
    int active = 1;
    int status = 0;
    pid_t child;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, system_handoff_active(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_handoff_active(&active));
    TEST_ASSERT_FALSE(active);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(NULL, "3", "4"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(&handoff, "invalid", "4"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_accept(&handoff, "3", "invalid"));

    build_path(ordinary_path, sizeof(ordinary_path), "not-running-executable");
    write_text(ordinary_path, "not the running executable\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          system_unlink_running_executable(ordinary_path));
    TEST_ASSERT_EQUAL_INT(0, access(ordinary_path, F_OK));
    TEST_ASSERT_EQUAL_INT(0, unlink(ordinary_path));

    build_path(lock_path, sizeof(lock_path), "handoff.lock");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    authority_fd = fcntl((int)lock.handle, F_DUPFD, STDERR_FILENO + 1);
    TEST_ASSERT_TRUE(authority_fd > STDERR_FILENO);
    TEST_ASSERT_EQUAL_INT(0, pipe(signal_fds));
    TEST_ASSERT_TRUE(signal_fds[0] > STDERR_FILENO);
    TEST_ASSERT_TRUE(signal_fds[1] > STDERR_FILENO);
    TEST_ASSERT_TRUE(snprintf(parent_signal_value, sizeof(parent_signal_value), "%d", signal_fds[0]) > 0);
    TEST_ASSERT_TRUE(
        snprintf(authority_value, sizeof(authority_value), "%d", authority_fd) > 0);

    /* Simulate parent exit without LOCK_UN. The duplicated descriptor must retain the same flock
     * authority for the accepting child. */
    original_fd = (int)lock.handle;
    TEST_ASSERT_EQUAL_INT(0, close(original_fd));
    lock.handle = -1;
    lock.mode = SYSTEM_LOCK_SHARED;
    lock.active = 0;
    TEST_ASSERT_EQUAL_INT(0, close(signal_fds[1]));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_handoff_accept(&handoff, parent_signal_value, authority_value));
    TEST_ASSERT_TRUE(handoff.active);

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        SystemLock child_lock = {0};
        CupError err =
            system_lock_acquire(&child_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);

        if (err == CUP_OK) {
            system_lock_release(&child_lock);
        }
        _exit(err == CUP_ERR_LOCK ? 0 : 1);
    }
    TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(NULL, &resumed, lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(&handoff, NULL, lock_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_handoff_acquire_lock(&handoff, &resumed, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_handoff_acquire_lock(&handoff, &resumed, lock_path));
    TEST_ASSERT_FALSE(handoff.active);
    TEST_ASSERT_TRUE(resumed.active);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, resumed.mode);
    system_handoff_release(&handoff);
    system_lock_release(&resumed);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&competing, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    system_lock_release(&competing);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_file(lock_path));
}

static void test_handoff_helper_detaches_standard_streams(void) {
    char helper[1024];
    char root[1024];
    char lock_path[1024];
    char ready_fifo[1024];
    char control_fifo[1024];
    char done_fifo[1024];
    char script_text[4096];
    int capture_fds[2] = {-1, -1};
    int ready_fd = -1;
    int control_fd = -1;
    int done_fd = -1;
    pid_t worker;
    int status = 0;
    struct pollfd wait_fd;
    char buffer[64];
    ssize_t count;

    build_path(helper, sizeof(helper), "handoff-detach-helper.sh");
    build_path(root, sizeof(root), "handoff-detach-root");
    build_path(ready_fifo, sizeof(ready_fifo), "handoff-ready.fifo");
    build_path(control_fifo, sizeof(control_fifo), "handoff-control.fifo");
    build_path(done_fifo, sizeof(done_fifo), "handoff-done.fifo");
    TEST_ASSERT_EQUAL_INT(0, mkdir(root, 0700));
    TEST_ASSERT_TRUE(snprintf(lock_path, sizeof(lock_path), "%s/cup.lock", root) > 0);
    TEST_ASSERT_EQUAL_INT(0, mkfifo(ready_fifo, 0600));
    TEST_ASSERT_EQUAL_INT(0, mkfifo(control_fifo, 0600));
    TEST_ASSERT_EQUAL_INT(0, mkfifo(done_fifo, 0600));

    ready_fd = open(ready_fifo, O_RDWR | O_NONBLOCK);
    control_fd = open(control_fifo, O_RDWR | O_NONBLOCK);
    done_fd = open(done_fifo, O_RDWR | O_NONBLOCK);
    TEST_ASSERT_TRUE(ready_fd >= 0);
    TEST_ASSERT_TRUE(control_fd >= 0);
    TEST_ASSERT_TRUE(done_fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, pipe(capture_fds));

    TEST_ASSERT_TRUE(snprintf(script_text,
                              sizeof(script_text),
                              "#!/bin/sh\n"
                              "printf 'inherited-output\\n'\n"
                              "printf 'ready\\n' > '%s'\n"
                              "IFS= read -r value < '%s'\n"
                              "printf 'done\\n' > '%s'\n",
                              ready_fifo,
                              control_fifo,
                              done_fifo) > 0);
    write_text(helper, script_text);
    TEST_ASSERT_EQUAL_INT(0, chmod(helper, 0755));

    worker = fork();
    TEST_ASSERT_TRUE(worker >= 0);
    if (worker == 0) {
        SystemLock worker_lock = {0};
        CupError err;

        (void)close(capture_fds[0]);
        if (dup2(capture_fds[1], STDOUT_FILENO) < 0 ||
            dup2(capture_fds[1], STDERR_FILENO) < 0) {
            _exit(2);
        }
        (void)close(capture_fds[1]);
        (void)close(ready_fd);
        (void)close(control_fd);
        (void)close(done_fd);
        err = system_lock_acquire(&worker_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) {
            err = system_start_update_helper(helper, root, "detach-token", &worker_lock);
        }
        _exit(err == CUP_OK && !worker_lock.active ? 0 : 1);
    }

    (void)close(capture_fds[1]);
    capture_fds[1] = -1;
    TEST_ASSERT_EQUAL_INT(worker, waitpid(worker, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    wait_fd.fd = ready_fd;
    wait_fd.events = POLLIN;
    wait_fd.revents = 0;
    TEST_ASSERT_EQUAL_INT(1, poll(&wait_fd, 1, 5000));
    TEST_ASSERT_TRUE((wait_fd.revents & POLLIN) != 0);
    TEST_ASSERT_TRUE(read(ready_fd, buffer, sizeof(buffer)) > 0);

    TEST_ASSERT_EQUAL_INT(0, fcntl(capture_fds[0], F_SETFL, O_NONBLOCK));
    count = read(capture_fds[0], buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT64(0, count);

    TEST_ASSERT_TRUE(write(control_fd, "continue\n", 9) > 0);
    wait_fd.fd = done_fd;
    wait_fd.events = POLLIN;
    wait_fd.revents = 0;
    TEST_ASSERT_EQUAL_INT(1, poll(&wait_fd, 1, 5000));
    TEST_ASSERT_TRUE((wait_fd.revents & POLLIN) != 0);
    TEST_ASSERT_TRUE(read(done_fd, buffer, sizeof(buffer)) > 0);

    (void)close(capture_fds[0]);
    (void)close(ready_fd);
    (void)close(control_fd);
    (void)close(done_fd);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(root, NULL));
    TEST_ASSERT_EQUAL_INT(0, unlink(helper));
    TEST_ASSERT_EQUAL_INT(0, unlink(ready_fifo));
    TEST_ASSERT_EQUAL_INT(0, unlink(control_fifo));
    TEST_ASSERT_EQUAL_INT(0, unlink(done_fifo));
}

static void test_handoff_helper_start(void) {
    char helper[1024];
    char update_marker[1024];
    char uninstall_marker[1024];
    char root[1024];
    char detached[1024];
    char lock_path[1024];
    char contents[4096];
    char script_text[4096];
    SystemLock lock = {0};
    pid_t worker;
    int status = 0;

    build_path(helper, sizeof(helper), "handoff-helper.sh");
    build_path(update_marker, sizeof(update_marker), "update-handoff.out");
    build_path(uninstall_marker, sizeof(uninstall_marker), "uninstall-handoff.out");
    build_path(root, sizeof(root), "handoff-root");
    build_path(detached, sizeof(detached), "handoff-detached");
    TEST_ASSERT_EQUAL_INT(0, mkdir(root, 0700));
    TEST_ASSERT_TRUE(snprintf(lock_path, sizeof(lock_path), "%s/cup.lock", root) > 0);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(NULL, root, "token", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(helper, NULL, "token", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(helper, root, NULL, &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_update_helper(helper, root, "token", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          system_start_uninstall_helper(
                              helper, root, NULL, "token", &lock));

    TEST_ASSERT_TRUE(snprintf(script_text,
                              sizeof(script_text),
                              "#!/bin/sh\n"
                              "printf '%%s\\n' \"$@\" > '%s'\n",
                              update_marker) > 0);
    write_text(helper, script_text);
    TEST_ASSERT_EQUAL_INT(0, chmod(helper, 0644));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          system_start_update_helper(helper, root, "token", &lock));
    TEST_ASSERT_TRUE(lock.active);
    system_lock_release(&lock);
    TEST_ASSERT_EQUAL_INT(0, chmod(helper, 0755));

    worker = fork();
    TEST_ASSERT_TRUE(worker >= 0);
    if (worker == 0) {
        SystemLock worker_lock = {0};
        CupError err =
            system_lock_acquire(&worker_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);

        if (err == CUP_OK) {
            err = system_start_update_helper(helper, root, "update-token", &worker_lock);
        }
        _exit(err == CUP_OK && !worker_lock.active ? 0 : 1);
    }
    TEST_ASSERT_EQUAL_INT(worker, waitpid(worker, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
    TEST_ASSERT_TRUE(wait_for_path(update_marker, 1));
    read_text(update_marker, contents, sizeof(contents));
    TEST_ASSERT_NOT_NULL(strstr(contents, "--internal-update-helper\n"));
    TEST_ASSERT_NOT_NULL(strstr(contents, root));
    TEST_ASSERT_NOT_NULL(strstr(contents, "update-token\n"));

    TEST_ASSERT_TRUE(snprintf(script_text,
                              sizeof(script_text),
                              "#!/bin/sh\n"
                              "printf '%%s\\n' \"$@\" > '%s'\n",
                              uninstall_marker) > 0);
    write_text(helper, script_text);
    TEST_ASSERT_EQUAL_INT(0, chmod(helper, 0755));

    worker = fork();
    TEST_ASSERT_TRUE(worker >= 0);
    if (worker == 0) {
        SystemLock worker_lock = {0};
        CupError err =
            system_lock_acquire(&worker_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);

        if (err == CUP_OK) {
            err = system_start_uninstall_helper(
                helper, root, detached, "uninstall-token", &worker_lock);
        }
        _exit(err == CUP_OK && !worker_lock.active ? 0 : 1);
    }
    TEST_ASSERT_EQUAL_INT(worker, waitpid(worker, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
    TEST_ASSERT_TRUE(wait_for_path(uninstall_marker, 1));
    read_text(uninstall_marker, contents, sizeof(contents));
    TEST_ASSERT_NOT_NULL(strstr(contents, "--internal-uninstall-helper\n"));
    TEST_ASSERT_NOT_NULL(strstr(contents, root));
    TEST_ASSERT_NOT_NULL(strstr(contents, detached));
    TEST_ASSERT_NOT_NULL(strstr(contents, "uninstall-token\n"));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE));
    system_lock_release(&lock);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(root, NULL));
    TEST_ASSERT_EQUAL_INT(0, unlink(helper));
    TEST_ASSERT_EQUAL_INT(0, unlink(update_marker));
    TEST_ASSERT_EQUAL_INT(0, unlink(uninstall_marker));
}

static void test_suite_cleanup(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(temp_dir, NULL));
}


void register_system_posix_tests(void) {
    const char *home = getenv("HOME");

    if (home != NULL) {
        int written = snprintf(original_home, sizeof(original_home), "%s", home);

        TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(original_home));
        had_home = 1;
    }
    TEST_ASSERT_NOT_NULL(
        test_make_temp_directory(temp_dir, sizeof(temp_dir), "cup-system-test"));

    RUN_TEST(test_home_process);
    RUN_TEST(test_path_and_walk);
    RUN_TEST(test_copy_move_temp);
    RUN_TEST(test_lock_contention);
    RUN_TEST(test_api_errors);
    RUN_TEST(test_extra_paths);
    RUN_TEST(test_trusted_operations_reject_symlinked_parent);
    RUN_TEST(test_private_directory_contract);
    RUN_TEST(test_remove_tree_path_forms);
    RUN_TEST(test_tree_depth_limit);
    RUN_TEST(test_identity_bound_path_removal);
    RUN_TEST(test_shared_script_primitives);
    RUN_TEST(test_handoff_primitives);
    RUN_TEST(test_handoff_helper_detaches_standard_streams);
    RUN_TEST(test_handoff_helper_start);
    RUN_TEST(test_suite_cleanup);
}
