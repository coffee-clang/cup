/* Exercises the detached CUP-update helper commit order and executable continuity. */

#include "cup_update_helper.h"

#include "constants.h"
#include "cup_update_journal.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static char root[MAX_PATH_LEN];
static char staging[MAX_PATH_LEN];
static int journal_cleared;
static int journal_clear_failures_remaining;
static int success_recorded;
static int failure_recorded;
static int lock_released;
static int replace_calls;
static int replace_fail_call;
static int recovery_calls;
static CupUpdateRecoveryMode recovery_mode;
static CupUpdateRecoveryResult recovery_result;
static int executable_calls;
static int read_only_calls;
static int writable_calls;
static CupError cleanup_result;
static CupError expected_failure_error;

static CupError write_path(char *buffer, size_t size, const char *relative) {
    return path_join(buffer, size, root, relative);
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(test_mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void read_file(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "rb");
    size_t count;

    TEST_ASSERT_NOT_NULL(file);
    count = fread(buffer, 1, size - 1, file);
    TEST_ASSERT_EQUAL_INT(0, ferror(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buffer[count] = '\0';
}

static void assert_file_text(const char *path, const char *expected) {
    char actual[32];

    read_file(path, actual, sizeof(actual));
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static void remove_tree_real(const char *path) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(path));
}

static void create_asset(const char *relative, const char *text) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(path, sizeof(path), relative));
    write_file(path, text);
}

static void create_staged_asset(const char *name) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), staging, name));
    write_file(path, "new");
}

void setUp(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];
    char path[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        template_path, sizeof(template_path), "cup-update-helper-unit"));
    TEST_ASSERT_TRUE(strlen(template_path) < sizeof(root));
    strcpy(root, template_path);

    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(path, sizeof(path), "bin"));
    make_dir(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(path, sizeof(path), "config"));
    make_dir(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(path, sizeof(path), "helpers"));
    make_dir(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(path, sizeof(path), "staging"));
    make_dir(path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, write_path(staging, sizeof(staging), "staging/cup-update-test"));
    make_dir(staging);

    create_asset("bin/" CUP_BINARY_FILENAME, "old");
    create_asset("helpers/" CUP_UNINSTALL_FILENAME, "old");
    create_asset("config/platform.sum", "old");
    create_asset("config/packages.cfg", "old");
    create_asset("config/install.cfg", "old");
    create_asset("config/common.sum", "old");

    create_staged_asset(CUP_UPDATE_BINARY_NEW);
    create_staged_asset(CUP_UPDATE_UNINSTALL_NEW);
    create_staged_asset(CUP_UPDATE_PLATFORM_CHECKSUMS_NEW);
    create_staged_asset(CUP_UPDATE_PACKAGES_NEW);
    create_staged_asset(CUP_UPDATE_INSTALL_POLICY_NEW);
    create_staged_asset(CUP_UPDATE_COMMON_CHECKSUMS_NEW);

    journal_cleared = 0;
    journal_clear_failures_remaining = 0;
    success_recorded = 0;
    failure_recorded = 0;
    lock_released = 0;
    replace_calls = 0;
    replace_fail_call = 0;
    recovery_calls = 0;
    recovery_mode = CUP_UPDATE_RECOVER_PRESERVE_BINARY;
    recovery_result = CUP_UPDATE_RECOVERY_ROLLED_BACK;
    executable_calls = 0;
    read_only_calls = 0;
    writable_calls = 0;
    cleanup_result = CUP_OK;
    expected_failure_error = CUP_ERR_TRANSACTION;
}

void tearDown(void) {
    remove_tree_real(root);
}

CupError layout_ensure_cup_assets(void) {
    return CUP_OK;
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return write_path(buffer, size, "bin/" CUP_BINARY_FILENAME);
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return write_path(buffer, size, "helpers/" CUP_UNINSTALL_FILENAME);
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return write_path(buffer, size, "config/platform.sum");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return write_path(buffer, size, "config/packages.cfg");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return write_path(buffer, size, "config/install.cfg");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return write_path(buffer, size, "config/common.sum");
}

CupError layout_get_cup_update_helper_path(char *buffer, size_t size) {
    return write_path(buffer, size, "helpers/" CUP_UPDATE_HELPER_FILENAME);
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return write_path(buffer, size, "cup.lock");
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TestPlatformStat status;

    if (test_stat_path(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (test_stat_is_regular(&status)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (test_stat_is_directory(&status)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError system_copy_file(const char *source, const char *destination) {
    char data[32];

    read_file(source, data, sizeof(data));
    write_file(destination, data);
    return CUP_OK;
}

static void assert_supporting_assets_are_new(void) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    assert_file_text(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_platform_checksums_path(path, sizeof(path)));
    assert_file_text(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(path, sizeof(path)));
    assert_file_text(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_install_policy_path(path, sizeof(path)));
    assert_file_text(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_common_checksums_path(path, sizeof(path)));
    assert_file_text(path, "new");
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    char binary[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    if (strcmp(destination, binary) == 0) {
        TEST_ASSERT_EQUAL_INT(
            CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
        TEST_ASSERT_TRUE(test_access_exists(marker));
        assert_file_text(binary, "old");
        assert_supporting_assets_are_new();
    } else {
        assert_file_text(binary, "old");
    }

    *state = SYSTEM_COMMIT_NOT_APPLIED;
    replace_calls++;
    if (replace_fail_call != 0 && replace_calls == replace_fail_call) {
        return CUP_ERR_FILESYSTEM;
    }
    if (test_access_exists(destination) && test_unlink(destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (rename(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    (void)path;
    TEST_ASSERT_EQUAL_INT(1, executable);
    executable_calls++;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    if (read_only) {
        read_only_calls++;
    } else {
        writable_calls++;
    }
    return CUP_OK;
}

CupError system_create_file_exclusive(const char *path, FILE **file) {
    if (file == NULL || test_access_exists(path)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file = fopen(path, "w+b");
    return *file == NULL ? CUP_ERR_FILESYSTEM : CUP_OK;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    return CUP_OK;
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    (void)path;
    (void)mode;
    lock->active = 1;
    return CUP_OK;
}

void system_lock_release(SystemLock *lock) {
    lock->active = 0;
    lock_released++;
}

CupError filesystem_remove_tree(const char *path) {
    if (cleanup_result != CUP_OK) {
        return cleanup_result;
    }
    remove_tree_real(path);
    return CUP_OK;
}

void cup_update_journal_init(CupUpdateJournal *journal) {
    memset(journal, 0, sizeof(*journal));
}

CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status) {
    cup_update_journal_init(journal);
    strcpy(journal->temporary_name, "cup-update-test");
    strcpy(journal->token, "token");
    strcpy(journal->version, "2.0.0");
    journal->phase = CUP_UPDATE_PHASE_SCHEDULED;
    *status = CUP_UPDATE_JOURNAL_LOADED;
    return CUP_OK;
}

CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
                                             char *buffer,
                                             size_t size) {
    (void)journal;
    return size > strlen(staging) ? (strcpy(buffer, staging), CUP_OK) : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code) {
    if (phase == CUP_UPDATE_PHASE_COMMITTING) {
        TEST_ASSERT_EQUAL_INT(0, error_code);
    } else {
        TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, phase);
        TEST_ASSERT_EQUAL_INT(expected_failure_error, error_code);
    }
    journal->phase = phase;
    journal->error_code = error_code;
    return CUP_OK;
}

CupError cup_update_journal_clear(void) {
    journal_cleared++;
    if (journal_clear_failures_remaining > 0) {
        journal_clear_failures_remaining--;
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError cup_update_journal_recover(const CupUpdateJournal *journal,
                                    CupUpdateRecoveryMode mode,
                                    CupUpdateRecoveryResult *result) {
    TEST_ASSERT_NOT_NULL(journal);
    recovery_calls++;
    recovery_mode = mode;
    if (result != NULL) {
        *result = recovery_result;
    }
    return CUP_OK;
}

CupError cup_update_result_write(CupUpdateResultStatus status,
                                 int error_code,
                                 const char *version) {
    TEST_ASSERT_EQUAL_STRING("2.0.0", version);
    if (status == CUP_UPDATE_RESULT_SUCCESS) {
        TEST_ASSERT_EQUAL_INT(0, error_code);
        success_recorded++;
    } else {
        TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_FAILED, status);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, error_code);
        failure_recorded++;
    }
    return CUP_OK;
}

static void make_closed_parent_signal(char *value, size_t size) {
#if defined(_WIN32)
    HANDLE read_handle = NULL;
    HANDLE write_handle = NULL;
    int written;

    TEST_ASSERT_TRUE(CreatePipe(&read_handle, &write_handle, NULL, 0));
    TEST_ASSERT_TRUE(CloseHandle(write_handle));
    written = snprintf(value, size, "%llu", (unsigned long long)(uintptr_t)read_handle);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < size);
#else
    int descriptors[2];
    int written;

    TEST_ASSERT_EQUAL_INT(0, pipe(descriptors));
    TEST_ASSERT_EQUAL_INT(0, close(descriptors[1]));
    written = snprintf(value, size, "%d", descriptors[0]);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < size);
#endif
}

static void test_commit_keeps_executable_continuously_available(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, journal_cleared);
    TEST_ASSERT_EQUAL_INT(1, success_recorded);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_released);
    TEST_ASSERT_FALSE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(1 + CUP_UNINSTALL_EXECUTABLE, executable_calls);
    TEST_ASSERT_EQUAL_INT(5, read_only_calls);
    TEST_ASSERT_EQUAL_INT(6, writable_calls);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "new");
    assert_supporting_assets_are_new();
}

static void test_cleanup_failure_does_not_turn_committed_update_into_failure(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    cleanup_result = CUP_ERR_FILESYSTEM;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, journal_cleared);
    TEST_ASSERT_EQUAL_INT(1, success_recorded);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_released);
    TEST_ASSERT_TRUE(test_access_exists(staging));

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "new");
    assert_supporting_assets_are_new();
}

static void test_recovery_finalization_records_committed_generation_as_success(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    journal_clear_failures_remaining = 1;
    expected_failure_error = CUP_ERR_FILESYSTEM;
    recovery_result = CUP_UPDATE_RECOVERY_FINALIZED;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, journal_cleared);
    TEST_ASSERT_EQUAL_INT(1, success_recorded);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVER_REPLACE_BINARY, recovery_mode);
    TEST_ASSERT_EQUAL_INT(1, lock_released);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "new");
    assert_supporting_assets_are_new();
}

static void test_failure_delegates_binary_rollback_to_detached_helper(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    replace_fail_call = 6;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(0, journal_cleared);
    TEST_ASSERT_EQUAL_INT(0, success_recorded);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVER_REPLACE_BINARY, recovery_mode);
    TEST_ASSERT_EQUAL_INT(1, lock_released);
    TEST_ASSERT_EQUAL_INT(CUP_UNINSTALL_EXECUTABLE, executable_calls);
    TEST_ASSERT_EQUAL_INT(5, read_only_calls);
    TEST_ASSERT_EQUAL_INT(6, writable_calls);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "old");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_commit_keeps_executable_continuously_available);
    RUN_TEST(test_cleanup_failure_does_not_turn_committed_update_into_failure);
    RUN_TEST(test_recovery_finalization_records_committed_generation_as_success);
    RUN_TEST(test_failure_delegates_binary_rollback_to_detached_helper);
    return UNITY_END();
}
