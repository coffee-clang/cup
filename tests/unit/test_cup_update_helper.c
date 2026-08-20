/* Exercises the detached cup update helper commit order and executable continuity. */

#include "cup_update_helper.h"

#include "constants.h"
#include "checksum.h"
#include "cup_assets.h"
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

static char root[MAX_PATH_LEN];
static char staging[MAX_PATH_LEN];
static int journal_cleared;
static int journal_clear_failures_remaining;
static int failure_recorded;
static int lock_released;
static int replace_calls;
static int copy_calls;
static int copy_fail_call;
static int copy_corrupt;
static int replace_fail_call;
static int recovery_calls;
static CupUpdateRecoveryMode recovery_mode;
static CupUpdateRecoveryResult recovery_result;
static int executable_calls;
static int read_only_calls;
static int writable_calls;
static int executable_fail_call;
static int read_only_fail_call;
static CupError marker_create_result;
static CupError sync_file_result;
static CupError sync_parent_result;
static CupError cleanup_result;
static CupError expected_failure_error;
static CupError layout_result;
static CupError journal_load_result;
static CupError staging_path_result;
static CupError lock_result;
static CupError recovery_error;
static CupError replace_fail_state_error;
static CupUpdateJournalStatus journal_status;
static int checksum_calls;
static int checksum_fail_call;
static int writable_calls_before_failure;
static int layout_calls;
static int layout_fail_call;
static int lock_calls;
static int lock_failures_remaining;
static int sleep_calls;
static int start_calls;
static int wait_calls;
static CupError start_helper_result;
static CupError wait_parent_result;
static CupError sleep_result;

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

static void reset_scenario(void) {
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
    failure_recorded = 0;
    lock_released = 0;
    replace_calls = 0;
    copy_calls = 0;
    copy_fail_call = 0;
    copy_corrupt = 0;
    replace_fail_call = 0;
    recovery_calls = 0;
    recovery_mode = CUP_UPDATE_RECOVER_PRESERVE_BINARY;
    recovery_result = CUP_UPDATE_RECOVERY_ROLLED_BACK;
    executable_calls = 0;
    read_only_calls = 0;
    writable_calls = 0;
    executable_fail_call = 0;
    read_only_fail_call = 0;
    marker_create_result = CUP_OK;
    sync_file_result = CUP_OK;
    sync_parent_result = CUP_OK;
    cleanup_result = CUP_OK;
    expected_failure_error = CUP_ERR_TRANSACTION;
    layout_result = CUP_OK;
    journal_load_result = CUP_OK;
    staging_path_result = CUP_OK;
    lock_result = CUP_OK;
    recovery_error = CUP_OK;
    replace_fail_state_error = CUP_OK;
    journal_status = CUP_UPDATE_JOURNAL_LOADED;
    checksum_calls = 0;
    checksum_fail_call = 0;
    writable_calls_before_failure = 0;
    layout_calls = 0;
    layout_fail_call = 0;
    lock_calls = 0;
    lock_failures_remaining = 0;
    sleep_calls = 0;
    start_calls = 0;
    wait_calls = 0;
    start_helper_result = CUP_OK;
    wait_parent_result = CUP_OK;
    sleep_result = CUP_OK;
}

void setUp(void) {
    reset_scenario();
}

static void restart_scenario(void) {
    remove_tree_real(root);
    reset_scenario();
}

void tearDown(void) {
    remove_tree_real(root);
}

static CupError next_layout_result(void) {
    layout_calls++;
    if (layout_result != CUP_OK ||
        (layout_fail_call != 0 && layout_calls == layout_fail_call)) {
        return layout_result != CUP_OK ? layout_result : CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

CupError layout_ensure_cup_assets(void) {
    return next_layout_result();
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "bin/" CUP_BINARY_FILENAME) : err;
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "helpers/" CUP_UNINSTALL_FILENAME) : err;
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "config/platform.sum") : err;
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "config/packages.cfg") : err;
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "config/install.cfg") : err;
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "config/common.sum") : err;
}

CupError layout_get_cup_update_helper_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK
               ? write_path(buffer, size, "helpers/" CUP_UPDATE_HELPER_FILENAME)
               : err;
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    CupError err = next_layout_result();
    return err == CUP_OK ? write_path(buffer, size, "cup.lock") : err;
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

    copy_calls++;
    if (copy_fail_call != 0 && copy_calls == copy_fail_call) {
        return CUP_ERR_FILESYSTEM;
    }
    read_file(source, data, sizeof(data));
    write_file(destination, copy_corrupt ? "corrupt" : data);
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular) {
    SystemPathKind kind;
    CupError err;

    if (is_regular == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    *is_regular = kind == SYSTEM_PATH_REGULAR_FILE;
    return CUP_OK;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    char data[32];
    char value;

    checksum_calls++;
    if (checksum_fail_call != 0 && checksum_calls == checksum_fail_call) {
        return CUP_ERR_FILESYSTEM;
    }
    if (hex == NULL || size < CHECKSUM_SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    read_file(path, data, sizeof(data));
    value = strcmp(data, "old") == 0 ? 'a' : 'b';
    memset(hex, value, CHECKSUM_SHA256_HEX_LENGTH);
    hex[CHECKSUM_SHA256_HEX_LENGTH] = '\0';
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
        if (replace_fail_state_error != CUP_OK) {
            *state = SYSTEM_COMMIT_APPLIED;
        }
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
    if (executable_fail_call != 0 && executable_calls == executable_fail_call) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    if (read_only) {
        read_only_calls++;
        if (read_only_fail_call != 0 && read_only_calls == read_only_fail_call) {
            return CUP_ERR_FILESYSTEM;
        }
    } else {
        writable_calls++;
        if (writable_calls_before_failure != 0 &&
            writable_calls == writable_calls_before_failure) {
            return CUP_ERR_FILESYSTEM;
        }
    }
    return CUP_OK;
}

CupError system_create_file_exclusive(const char *path, FILE **file) {
    if (marker_create_result != CUP_OK) {
        return marker_create_result;
    }
    if (file == NULL || test_access_exists(path)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file = fopen(path, "w+b");
    return *file == NULL ? CUP_ERR_FILESYSTEM : CUP_OK;
}

CupError system_sync_file(FILE *file) {
    if (sync_file_result != CUP_OK) {
        return sync_file_result;
    }
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    return sync_parent_result;
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    (void)path;
    (void)mode;
    lock_calls++;
    if (lock_result != CUP_OK) {
        return lock_result;
    }
    if (lock_failures_remaining > 0) {
        lock_failures_remaining--;
        return CUP_ERR_LOCK;
    }
    lock->active = 1;
    return CUP_OK;
}

void system_lock_release(SystemLock *lock) {
    lock->active = 0;
    lock_released++;
}

CupError system_start_cup_update_helper(const char *helper, const char *token) {
    if (helper == NULL || helper[0] == '\0' || token == NULL || token[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    start_calls++;
    return start_helper_result;
}

CupError system_wait_for_parent_exit(const char *wait_value) {
    if (wait_value == NULL || wait_value[0] == '\0' || strcmp(wait_value, "invalid") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    wait_calls++;
    return wait_parent_result;
}

CupError system_sleep_milliseconds(unsigned int milliseconds) {
    TEST_ASSERT_EQUAL_UINT(100, milliseconds);
    sleep_calls++;
    return sleep_result;
}

CupError filesystem_apply_required_permissions(const char *path, int executable, int read_only) {
    CupError err = CUP_OK;

    if (executable) {
        err = system_set_executable(path, 1);
    }
    if (err == CUP_OK && read_only) {
        err = system_set_read_only(path, 1);
    }
    return err;
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

CupError layout_root_snapshot_validate(void) {
    return CUP_OK;
}

CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status) {
    if (journal_load_result != CUP_OK) {
        return journal_load_result;
    }
    cup_update_journal_init(journal);
    strcpy(journal->temporary_name, "cup-update-test");
    strcpy(journal->token, "token");
    strcpy(journal->version, "2.0.0");
    journal->phase = CUP_UPDATE_PHASE_SCHEDULED;
    journal->file_identity.valid = 1;
    journal->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    *status = journal_status;
    return CUP_OK;
}

CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
                                             char *buffer,
                                             size_t size) {
    (void)journal;
    if (staging_path_result != CUP_OK) {
        return staging_path_result;
    }
    return size > strlen(staging) ? (strcpy(buffer, staging), CUP_OK) : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_update_write_generation_marker(const char *staging_path,
                                            const char *version,
                                            const char *staged_binary) {
    char marker[MAX_PATH_LEN];
    FILE *file = NULL;
    int failed = 0;

    (void)version;
    (void)staged_binary;
    if (path_join(marker, sizeof(marker), staging_path, CUP_UPDATE_COMMITTED) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (system_create_file_exclusive(marker, &file) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    if (fputs("generation\n", file) == EOF || system_sync_file(file) != CUP_OK) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed || system_sync_parent_directory(marker) != CUP_OK) {
        (void)test_unlink(marker);
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code) {
    if (phase == CUP_UPDATE_PHASE_COMMITTING) {
        TEST_ASSERT_EQUAL_INT(0, error_code);
    } else {
        TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, phase);
        TEST_ASSERT_EQUAL_INT(expected_failure_error, error_code);
        failure_recorded++;
    }
    journal->phase = phase;
    journal->error_code = error_code;
    return CUP_OK;
}



CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, expected_identity->kind);
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
    return recovery_error;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    TEST_ASSERT_NOT_NULL(inspection);
    memset(inspection, 0, sizeof(*inspection));
    inspection->binary = CUP_ASSET_VALID;
    inspection->helper = CUP_ASSET_MISSING;
    inspection->catalog = CUP_ASSET_VALID;
    inspection->install_policy = CUP_ASSET_VALID;
    inspection->uninstall = CUP_ASSET_VALID;
    inspection->common_checksums = CUP_ASSET_VALID;
    inspection->platform_checksums = CUP_ASSET_VALID;
    return CUP_OK;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    return inspection != NULL && inspection->binary == CUP_ASSET_VALID &&
           inspection->catalog == CUP_ASSET_VALID &&
           inspection->install_policy == CUP_ASSET_VALID &&
           inspection->uninstall == CUP_ASSET_VALID &&
           inspection->common_checksums == CUP_ASSET_VALID &&
           inspection->platform_checksums == CUP_ASSET_VALID;
}

static void make_closed_parent_signal(char *value, size_t size) {
    int written;

    written = snprintf(value, size, "closed-parent");
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < size);
}

static void test_start_rejects_invalid_token(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_helper_start(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_helper_start(""));

    layout_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_helper_start("token"));
}

static void test_start_creates_native_handoff(void) {
    layout_calls = 0;
    start_helper_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_start("token"));
    TEST_ASSERT_EQUAL_INT(1, start_calls);

    start_helper_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_start("token"));
    TEST_ASSERT_EQUAL_INT(2, start_calls);
}

static void test_prepare_reports_path_and_hash_failures(void) {
    char helper[MAX_PATH_LEN];

    layout_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_update_helper_prepare());

    layout_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_helper_path(helper, sizeof(helper)));
    write_file(helper, "old");
    checksum_fail_call = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_prepare());

    checksum_calls = 0;
    checksum_fail_call = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_prepare());

    checksum_calls = 0;
    checksum_fail_call = 3;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_prepare());
}

static void test_run_rejects_parent_and_journal_failures(void) {
    char wait_value[32];

    make_closed_parent_signal(wait_value, sizeof(wait_value));
    wait_parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_update_helper_run("token", wait_value));

    wait_parent_result = CUP_OK;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    journal_load_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_helper_run("token", wait_value));

    journal_load_result = CUP_OK;
    journal_status = CUP_UPDATE_JOURNAL_MISSING;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_helper_run("token", wait_value));

    journal_status = CUP_UPDATE_JOURNAL_LOADED;
    staging_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          cup_update_helper_run("token", wait_value));
}

static void test_lock_failure_preserves_scheduled_journal(void) {
    char wait_value[32];

    lock_result = CUP_ERR_FILESYSTEM;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(0, lock_released);
}

static void test_marker_creation_and_sync_failures_are_recorded(void) {
    char wait_value[32];

    marker_create_result = CUP_ERR_FILESYSTEM;
    expected_failure_error = CUP_ERR_FILESYSTEM;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);

    restart_scenario();
    sync_file_result = CUP_ERR_FILESYSTEM;
    expected_failure_error = CUP_ERR_COMMIT;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
}

static void test_backup_and_replace_state_failures_are_distinguished(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(path));
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(path, 0700));
    expected_failure_error = CUP_ERR_VALIDATION;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          cup_update_helper_run("token", wait_value));

    restart_scenario();
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(path, sizeof(path), staging, CUP_UPDATE_UNINSTALL_OLD));
    write_file(path, "existing");
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_helper_run("token", wait_value));

    restart_scenario();
    replace_fail_call = 1;
    replace_fail_state_error = CUP_ERR_COMMIT;
    expected_failure_error = CUP_ERR_COMMIT;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          cup_update_helper_run("token", wait_value));
}


static void test_missing_destination_is_installed_from_absent_evidence(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(path));
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    assert_file_text(path, "new");
    TEST_ASSERT_EQUAL_INT(6, replace_calls);
}

static void test_destination_permission_reset_failure_prevents_replace(void) {
    char wait_value[32];

    writable_calls_before_failure = 1;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
}

static void test_failed_recovery_preserves_original_error(void) {
    char wait_value[32];

    marker_create_result = CUP_ERR_FILESYSTEM;
    expected_failure_error = CUP_ERR_FILESYSTEM;
    recovery_error = CUP_ERR_ROLLBACK;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
}

static void test_asset_path_failures_abort_before_commit(void) {
    char wait_value[32];
    int fail_call;

    for (fail_call = 1; fail_call <= 7; ++fail_call) {
        if (fail_call != 1) {
            restart_scenario();
        }
        layout_fail_call = fail_call;
        expected_failure_error = fail_call == 1 ? CUP_ERR_LOCK : CUP_ERR_TRANSACTION;
        make_closed_parent_signal(wait_value, sizeof(wait_value));
        TEST_ASSERT_EQUAL_INT(
            expected_failure_error, cup_update_helper_run("token", wait_value));
        TEST_ASSERT_EQUAL_INT(0, failure_recorded);
        TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    }
}

static void test_lock_retry_and_exhaustion(void) {
    char wait_value[32];

    lock_failures_remaining = 2;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(3, lock_calls);
    TEST_ASSERT_EQUAL_INT(2, sleep_calls);

    restart_scenario();
    lock_failures_remaining = 600;
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(600, lock_calls);
    TEST_ASSERT_EQUAL_INT(600, sleep_calls);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
}

static void test_prepare_reuses_matching_helper(void) {
    char helper[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_helper_path(helper, sizeof(helper)));
    write_file(helper, "old");

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_prepare());
    TEST_ASSERT_EQUAL_INT(0, copy_calls);
    TEST_ASSERT_EQUAL_INT(1, executable_calls);
    assert_file_text(helper, "old");
}

static void test_prepare_replaces_missing_or_stale_helper(void) {
    char helper[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_helper_path(helper, sizeof(helper)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_prepare());
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
    assert_file_text(helper, "old");

    write_file(helper, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_prepare());
    TEST_ASSERT_EQUAL_INT(2, copy_calls);
    assert_file_text(helper, "old");
}

static void test_commit_keeps_executable_continuously_available(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, journal_cleared);
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
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_released);
    TEST_ASSERT_TRUE(test_access_exists(staging));

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "new");
    assert_supporting_assets_are_new();
}

static void test_recovery_finalizes_committed_generation(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    journal_clear_failures_remaining = 1;
    expected_failure_error = CUP_ERR_FILESYSTEM;
    recovery_result = CUP_UPDATE_RECOVERY_FINALIZED;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, journal_cleared);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
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

static void test_prepare_rejects_mismatched_copy(void) {
    copy_corrupt = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, cup_update_helper_prepare());
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
}

static void test_prepare_rejects_non_regular_helper(void) {
    char helper[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_helper_path(helper, sizeof(helper)));
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(helper, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, cup_update_helper_prepare());
    TEST_ASSERT_EQUAL_INT(0, copy_calls);
}

static void test_prepare_reports_copy_and_permission_failures(void) {
    char helper[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_helper_path(helper, sizeof(helper)));
    copy_fail_call = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_prepare());
    TEST_ASSERT_FALSE(test_access_exists(helper));

    copy_fail_call = 0;
    executable_fail_call = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_update_helper_prepare());
    TEST_ASSERT_TRUE(test_access_exists(helper));
}

static void test_run_rejects_invalid_handoff(void) {
    char wait_value[32];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_helper_run("token", "invalid"));
    make_closed_parent_signal(wait_value, sizeof(wait_value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_helper_run("other", wait_value));
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
}

static void test_missing_staged_asset_fails_validation(void) {
    char wait_value[32];
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(path, sizeof(path), staging, CUP_UPDATE_PACKAGES_NEW));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(path));
    expected_failure_error = CUP_ERR_VALIDATION;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(0, copy_calls);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
}

static void test_backup_copy_failure_aborts_before_commit(void) {
    char wait_value[32];
    char binary[MAX_PATH_LEN];

    copy_fail_call = 1;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
    TEST_ASSERT_EQUAL_INT(0, replace_calls);
    TEST_ASSERT_EQUAL_INT(0, recovery_calls);
    TEST_ASSERT_EQUAL_INT(0, failure_recorded);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    assert_file_text(binary, "old");
}

static void test_marker_durability_failure_is_commit_error(void) {
    char wait_value[32];

    sync_parent_result = CUP_ERR_FILESYSTEM;
    expected_failure_error = CUP_ERR_COMMIT;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(5, replace_calls);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
}

static void test_permission_failure_after_replace_is_commit_error(void) {
    char wait_value[32];

    read_only_fail_call = 1;
    expected_failure_error = CUP_ERR_COMMIT;
    make_closed_parent_signal(wait_value, sizeof(wait_value));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, cup_update_helper_run("token", wait_value));
    TEST_ASSERT_EQUAL_INT(1, replace_calls);
    TEST_ASSERT_EQUAL_INT(1, recovery_calls);
    TEST_ASSERT_EQUAL_INT(1, failure_recorded);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_start_rejects_invalid_token);
    RUN_TEST(test_start_creates_native_handoff);
    RUN_TEST(test_asset_path_failures_abort_before_commit);
    RUN_TEST(test_lock_retry_and_exhaustion);
    RUN_TEST(test_prepare_reuses_matching_helper);
    RUN_TEST(test_prepare_reports_path_and_hash_failures);
    RUN_TEST(test_prepare_replaces_missing_or_stale_helper);
    RUN_TEST(test_prepare_rejects_mismatched_copy);
    RUN_TEST(test_prepare_rejects_non_regular_helper);
    RUN_TEST(test_prepare_reports_copy_and_permission_failures);
    RUN_TEST(test_run_rejects_invalid_handoff);
    RUN_TEST(test_run_rejects_parent_and_journal_failures);
    RUN_TEST(test_lock_failure_preserves_scheduled_journal);
    RUN_TEST(test_missing_staged_asset_fails_validation);
    RUN_TEST(test_backup_copy_failure_aborts_before_commit);
    RUN_TEST(test_marker_durability_failure_is_commit_error);
    RUN_TEST(test_marker_creation_and_sync_failures_are_recorded);
    RUN_TEST(test_backup_and_replace_state_failures_are_distinguished);
    RUN_TEST(test_missing_destination_is_installed_from_absent_evidence);
    RUN_TEST(test_destination_permission_reset_failure_prevents_replace);
    RUN_TEST(test_permission_failure_after_replace_is_commit_error);
    RUN_TEST(test_commit_keeps_executable_continuously_available);
    RUN_TEST(test_cleanup_failure_does_not_turn_committed_update_into_failure);
    RUN_TEST(test_recovery_finalizes_committed_generation);
    RUN_TEST(test_failure_delegates_binary_rollback_to_detached_helper);
    RUN_TEST(test_failed_recovery_preserves_original_error);
    return UNITY_END();
}
