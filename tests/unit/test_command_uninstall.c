/*
 * Test focus: Exercises canonical-root validation, marker creation and detached uninstall
 * startup through system boundaries.
 */

#include "cup_assets.h"
#include "commands.h"
#include "error.h"
#include "layout.h"
#include "system.h"
#include "runtime_journal.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static int root_is_directory;
static CupError inspect_root_result;
static CupError lock_result;
static CupError transaction_load_result;
static RuntimeJournalKind journal_kind;
static CupError find_uninstall_result;
static CupError marker_create_result;
static CupError sync_file_result;
static CupError sync_parent_result;
static CupError helper_result;
static int lock_release_calls;
static int remove_file_calls;
static int helper_calls;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    root_is_directory = 1;
    inspect_root_result = CUP_OK;
    lock_result = CUP_OK;
    transaction_load_result = CUP_OK;
    journal_kind = RUNTIME_JOURNAL_MISSING;
    find_uninstall_result = CUP_OK;
    marker_create_result = CUP_OK;
    sync_file_result = CUP_OK;
    sync_parent_result = CUP_OK;
    helper_result = CUP_OK;
    lock_release_calls = 0;
    remove_file_calls = 0;
    helper_calls = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static CupError test_path(char *buffer, size_t size, const char *name) {
    return buffer_write_result(snprintf(buffer, size, "%s/%s", temp_dir, name), size);
}

static void provide_input(const char *text) {
    char path[1024];
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/input", temp_dir) > 0);
    file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(text, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_NOT_NULL(freopen(path, "r", stdin));
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_root(char *buffer, size_t size) {
    return test_path(buffer, size, "root");
}

CupError system_is_directory(const char *path, int *is_directory) {
    TEST_ASSERT_NOT_NULL(path);
    if (is_directory != NULL) {
        *is_directory = root_is_directory;
    }
    return inspect_root_result;
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return test_path(buffer, size, "cup.lock");
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    if (lock_result == CUP_OK) {
        lock->active = 1;
    }
    return lock_result;
}

void system_lock_release(SystemLock *lock) {
    if (lock != NULL && lock->active) {
        lock->active = 0;
    }
    lock_release_calls++;
}

CupError runtime_journal_detect(RuntimeJournalKind *kind) {
    TEST_ASSERT_NOT_NULL(kind);
    *kind = journal_kind;
    return transaction_load_result;
}

CupError cup_assets_find_uninstall(char *path, size_t size, CupAssetsSource *source) {
    if (find_uninstall_result != CUP_OK) {
        return find_uninstall_result;
    }
    TEST_ASSERT_NOT_NULL(source);
    *source = CUP_ASSETS_SOURCE_INSTALLED;
    return test_path(path, size, "uninstall.sh");
}

CupError layout_get_uninstall_marker_path(char *buffer, size_t size) {
    return test_path(buffer, size, "uninstall.pending");
}

CupError system_create_file_exclusive(const char *path, FILE **file) {
    if (marker_create_result != CUP_OK) {
        return marker_create_result;
    }
    TEST_ASSERT_NOT_NULL(file);
    *file = fopen(path, "wx");
    return *file != NULL ? CUP_OK : CUP_ERR_LOCK;
}

CupError system_sync_file(FILE *file) {
    TEST_ASSERT_NOT_NULL(file);
    return sync_file_result == CUP_OK && fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    TEST_ASSERT_NOT_NULL(path);
    return sync_parent_result;
}

CupError system_remove_file(const char *path) {
    TEST_ASSERT_NOT_NULL(path);
    remove_file_calls++;
    return unlink(path) == 0 || access(path, F_OK) != 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

unsigned long system_get_process_id(void) {
    return 4321;
}

CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                unsigned long parent_pid) {
    TEST_ASSERT_NOT_NULL(cup_root);
    TEST_ASSERT_NOT_NULL(uninstall_script);
    TEST_ASSERT_EQUAL_INT(4321, parent_pid);
    helper_calls++;
    return helper_result;
}

static void test_invalid_runtime(void) {
    provide_input("y\n");
    inspect_root_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, lock_release_calls);

    reset_scenario();
    provide_input("y\n");
    root_is_directory = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
}

static void test_pending_or_missing(void) {
    provide_input("y\n");
    journal_kind = RUNTIME_JOURNAL_PACKAGE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    provide_input("y\n");
    transaction_load_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));

    reset_scenario();
    provide_input("y\n");
    find_uninstall_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
}

static void test_confirmation(void) {
    provide_input("n\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("Y\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_file_calls);

    {
        char marker[1024];
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_marker_path(marker, sizeof(marker)));
        TEST_ASSERT_EQUAL_INT(0, unlink(marker));
    }
}

static void test_marker_cleanup(void) {
    provide_input("y\n");
    marker_create_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    sync_file_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_TRUE(remove_file_calls >= 1);

    reset_scenario();
    provide_input("y\n");
    helper_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_file_calls);
}

/* Suite registration. */

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-uninstall-test"));
    UNITY_BEGIN();
    RUN_TEST(test_invalid_runtime);
    RUN_TEST(test_pending_or_missing);
    RUN_TEST(test_confirmation);
    RUN_TEST(test_marker_cleanup);
    return UNITY_END();
}
