/*
 * Exercises canonical-root validation, transaction journal creation and detached
 * uninstall startup through controlled system boundaries.
 */

#include "cup_assets.h"
#include "constants.h"
#include "commands.h"
#include "error.h"
#include "interrupt.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "uninstall_journal.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static int root_is_directory;
static CupError root_path_result;
static CupError inspect_root_result;
static CupError lock_path_result;
static CupError lock_result;
static CupError existing_lock_result;
static CupError ensure_root_result;
static CupError journal_detect_result;
static RuntimeJournalKind journal_kind;
static CupError find_uninstall_result;
static CupError unique_path_result;
static int unique_path_has_expected_prefix;
static CupError journal_begin_result;
static CupError helper_result;
static CupError journal_clear_result;
static CupError journal_load_result;
static UninstallJournalStatus journal_load_status;
static UninstallJournal loaded_journal;
static int lock_release_calls;
static int existing_lock_calls;
static int unique_path_calls;
static int journal_begin_calls;
static int helper_calls;
static int journal_clear_calls;
static CupError interrupt_results[3];
static size_t interrupt_calls;
static int interrupt_pending;

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    root_is_directory = 1;
    root_path_result = CUP_OK;
    inspect_root_result = CUP_OK;
    lock_path_result = CUP_OK;
    lock_result = CUP_OK;
    existing_lock_result = CUP_OK;
    ensure_root_result = CUP_OK;
    journal_detect_result = CUP_OK;
    journal_kind = RUNTIME_JOURNAL_MISSING;
    find_uninstall_result = CUP_OK;
    unique_path_result = CUP_OK;
    unique_path_has_expected_prefix = 1;
    journal_begin_result = CUP_OK;
    helper_result = CUP_OK;
    journal_clear_result = CUP_OK;
    journal_load_result = CUP_OK;
    journal_load_status = UNINSTALL_JOURNAL_LOADED;
    memset(&loaded_journal, 0, sizeof(loaded_journal));
    loaded_journal.phase = UNINSTALL_PHASE_SCHEDULED;
    loaded_journal.stage = UNINSTALL_STAGE_HANDOFF;
    loaded_journal.file_identity.valid = 1;
    loaded_journal.file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    strcpy(loaded_journal.temporary_name, ".cup-uninstall-123-456-0.tmp");
    strcpy(loaded_journal.token, "123-456-0.tmp");
    lock_release_calls = 0;
    existing_lock_calls = 0;
    unique_path_calls = 0;
    journal_begin_calls = 0;
    helper_calls = 0;
    journal_clear_calls = 0;
    interrupt_results[0] = CUP_OK;
    interrupt_results[1] = CUP_OK;
    interrupt_results[2] = CUP_OK;
    interrupt_calls = 0;
    interrupt_pending = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

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

int interrupt_requested(void) {
    return interrupt_pending;
}

CupError interrupt_safe_point(void) {
    CupError result = interrupt_results[interrupt_calls < 3 ? interrupt_calls : 2];
    interrupt_calls++;
    return result;
}

CupError layout_root_snapshot_validate(void) {
    return CUP_OK;
}

CupError layout_get_root(char *buffer, size_t size) {
    if (root_path_result != CUP_OK) {
        return root_path_result;
    }
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
    if (lock_path_result != CUP_OK) {
        return lock_path_result;
    }
    return test_path(buffer, size, "cup.lock");
}

CupError layout_ensure_root(void) {
    return ensure_root_result;
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

CupError system_lock_acquire_existing(SystemLock *lock,
                                      const char *path,
                                      SystemLockMode mode) {
    existing_lock_calls++;
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_STRING("cup.lock", path_last_segment(path));
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    if (existing_lock_result == CUP_OK) {
        lock->active = 1;
    }
    return existing_lock_result;
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
    return journal_detect_result;
}



CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, expected_identity->kind);
    journal_clear_calls++;
    return journal_clear_result;
}

CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status) {
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_NOT_NULL(status);
    *journal = loaded_journal;
    *status = journal_load_status;
    return journal_load_result;
}

CupError cup_assets_find_uninstall(char *path, size_t size) {
    if (find_uninstall_result != CUP_OK) {
        return find_uninstall_result;
    }
    return test_path(path, size, CUP_UNINSTALL_FILENAME);
}

CupError system_make_unique_temp_path(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    unique_path_calls++;
    TEST_ASSERT_TRUE(path_equal(temp_dir, directory));
    TEST_ASSERT_EQUAL_STRING(".cup-uninstall", prefix);
    if (unique_path_result != CUP_OK) {
        return unique_path_result;
    }
    return buffer_write_result(
        snprintf(path,
                 path_size,
                 "%s/%s",
                 directory,
                 unique_path_has_expected_prefix ? ".cup-uninstall-123-456-0.tmp"
                                                 : "unexpected-token"),
        path_size);
}

CupError uninstall_journal_begin(const char *temporary_path, const char *token) {
    journal_begin_calls++;
    TEST_ASSERT_EQUAL_STRING(".cup-uninstall-123-456-0.tmp", path_last_segment(temporary_path));
    TEST_ASSERT_EQUAL_STRING("123-456-0.tmp", token);
    return journal_begin_result;
}

CupError system_start_uninstall(const char *cup_root,
                                const char *uninstall_script,
                                const char *detached_root,
                                const char *lock_path) {
    TEST_ASSERT_NOT_NULL(cup_root);
    TEST_ASSERT_NOT_NULL(uninstall_script);
    TEST_ASSERT_NOT_NULL(detached_root);
    TEST_ASSERT_NOT_NULL(lock_path);
    TEST_ASSERT_EQUAL_STRING("cup.lock", path_last_segment(lock_path));
    TEST_ASSERT_EQUAL_STRING(".cup-uninstall-123-456-0.tmp",
                             path_last_segment(detached_root));
    helper_calls++;
    return helper_result;
}

static void test_invalid_runtime(void) {
    provide_input("y\n");
    root_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, lock_release_calls);

    reset_scenario();
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
    lock_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    ensure_root_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_pending_or_missing(void) {
    provide_input("y\n");
    journal_kind = RUNTIME_JOURNAL_PACKAGE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    provide_input("y\n");
    journal_detect_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));

    reset_scenario();
    provide_input("y\n");
    find_uninstall_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
}

static void test_confirmation_and_success(void) {
    provide_input("");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, unique_path_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("n\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, unique_path_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("Y\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, unique_path_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
    TEST_ASSERT_EQUAL_INT(0, journal_clear_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_uninstall(1));
    TEST_ASSERT_EQUAL_INT(1, unique_path_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
}


static void test_interrupt_safe_points(void) {
    interrupt_results[0] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, command_uninstall(1));
    TEST_ASSERT_EQUAL_INT(0, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    interrupt_results[1] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    interrupt_results[2] = CUP_ERR_INTERRUPT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_clear_calls);
}

static void test_journal_and_handoff_failures(void) {
    provide_input("y\n");
    unique_path_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    unique_path_has_expected_prefix = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);

    reset_scenario();
    provide_input("y\n");
    journal_begin_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, journal_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
    TEST_ASSERT_EQUAL_INT(0, journal_clear_calls);

    reset_scenario();
    provide_input("y\n");
    helper_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
    TEST_ASSERT_EQUAL_INT(1, existing_lock_calls);
    TEST_ASSERT_EQUAL_INT(1, journal_clear_calls);

    reset_scenario();
    provide_input("y\n");
    helper_result = CUP_ERR_FILESYSTEM;
    loaded_journal.phase = UNINSTALL_PHASE_FAILED;
    loaded_journal.stage = UNINSTALL_STAGE_PARENT_WAIT;
    loaded_journal.error_code = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(0, journal_clear_calls);

    reset_scenario();
    provide_input("y\n");
    helper_result = CUP_ERR_FILESYSTEM;
    existing_lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, existing_lock_calls);
    TEST_ASSERT_EQUAL_INT(0, journal_clear_calls);

    reset_scenario();
    provide_input("y\n");
    helper_result = CUP_ERR_FILESYSTEM;
    journal_clear_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_uninstall(0));
    TEST_ASSERT_EQUAL_INT(1, journal_clear_calls);
}

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-uninstall-test"));
    UNITY_BEGIN();
    RUN_TEST(test_invalid_runtime);
    RUN_TEST(test_pending_or_missing);
    RUN_TEST(test_confirmation_and_success);
    RUN_TEST(test_interrupt_safe_points);
    RUN_TEST(test_journal_and_handoff_failures);
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(temp_dir));
    return UNITY_END();
}
