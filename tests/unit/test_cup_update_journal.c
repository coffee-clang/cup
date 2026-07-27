/* Exercises the strict deferred cup update journal and persisted result. */

#include "cup_assets.h"
#include "cup_update_journal.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char root[MAX_PATH_LEN];
static CupError replace_result;
static SystemCommitState replace_state;
static CupError permission_result;
static CupError remove_tree_result;
static int cup_assets_valid;
static int remove_tree_calls;
static int executable_calls;
static int read_only_calls;
static int writable_calls;
static char last_executable_path[MAX_PATH_LEN];
static char replaced_paths[8][MAX_PATH_LEN];
static int replaced_path_count;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree_real(const char *path) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(path));
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

static void assert_file_text(const char *path, const char *expected) {
    char buffer[64];
    FILE *file = fopen(path, "rb");
    size_t length;

    TEST_ASSERT_NOT_NULL(file);
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    TEST_ASSERT_EQUAL_INT(0, ferror(file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buffer[length] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected, buffer);
}

static void reset_scenario(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];
    char staging[MAX_PATH_LEN];
    char bin[MAX_PATH_LEN];
    char config[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        template_path, sizeof(template_path), "cup-update-journal-unit"));
    TEST_ASSERT_TRUE(strlen(template_path) < sizeof(root));
    strcpy(root, template_path);
    join_test_path(staging, sizeof(staging), root, "staging");
    join_test_path(bin, sizeof(bin), root, "bin");
    join_test_path(config, sizeof(config), root, "config");
    make_dir(staging);
    make_dir(bin);
    make_dir(config);

    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    permission_result = CUP_OK;
    remove_tree_result = CUP_OK;
    cup_assets_valid = 1;
    remove_tree_calls = 0;
    executable_calls = 0;
    read_only_calls = 0;
    writable_calls = 0;
    last_executable_path[0] = '\0';
    replaced_path_count = 0;
    memset(replaced_paths, 0, sizeof(replaced_paths));
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
    remove_tree_real(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_root(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "%s", root), size);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "transaction.txt");
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return path_join(buffer, size, root, "staging");
}

CupError layout_get_cup_update_result_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "cup-update-result.txt");
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/" CUP_BINARY_FILENAME);
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/" CUP_UNINSTALL_FILENAME);
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/SHA256SUMS");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/packages.cfg");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/install.cfg");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "config/SHA256SUMS.common");
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    return test_create_temp_file(directory, prefix, path, path_size, file) == 0
               ? CUP_OK
               : CUP_ERR_TEMPORARY;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (replaced_path_count < (int)(sizeof(replaced_paths) / sizeof(replaced_paths[0]))) {
        TEST_ASSERT_TRUE(strlen(destination) < sizeof(replaced_paths[0]));
        strcpy(replaced_paths[replaced_path_count], destination);
        replaced_path_count++;
    }
    if (replace_result != CUP_OK) {
        *state = replace_state;
        return replace_result;
    }
    if (test_replace_file(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = replace_state;
    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    *exists = test_access_exists(path);
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    return CUP_OK;
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

CupError system_set_executable(const char *path, int executable) {
    executable_calls++;
    TEST_ASSERT_EQUAL_INT(1, executable);
    TEST_ASSERT_TRUE(strlen(path) < sizeof(last_executable_path));
    strcpy(last_executable_path, path);
    return permission_result;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    if (read_only) {
        read_only_calls++;
        return permission_result;
    }
    writable_calls++;
    return CUP_OK;
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
    remove_tree_calls++;
    if (remove_tree_result != CUP_OK) {
        return remove_tree_result;
    }
    remove_tree_real(path);
    return CUP_OK;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    memset(inspection, 0, sizeof(*inspection));
    return CUP_OK;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    (void)inspection;
    return cup_assets_valid;
}

static void make_staging(const char *name, char *path, size_t size) {
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, size, staging, name));
    make_dir(path);
}

static void write_journal(const char *text) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    write_file(path, text);
}

static void create_destination_files(void) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_platform_checksums_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_install_policy_path(path, sizeof(path)));
    write_file(path, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_common_checksums_path(path, sizeof(path)));
    write_file(path, "new");
}

static void create_backups(const char *staging) {
    const char *names[] = {CUP_UPDATE_BINARY_OLD,
                           CUP_UPDATE_UNINSTALL_OLD,
                           CUP_UPDATE_PLATFORM_CHECKSUMS_OLD,
                           CUP_UPDATE_PACKAGES_OLD,
                           CUP_UPDATE_INSTALL_POLICY_OLD,
                           CUP_UPDATE_COMMON_CHECKSUMS_OLD};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[MAX_PATH_LEN];
        TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), staging, names[i]));
        write_file(path, "old");
    }
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_model_and_begin(void) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);
    TEST_ASSERT_EQUAL_STRING("scheduled", cup_update_phase_name(CUP_UPDATE_PHASE_SCHEDULED));
    TEST_ASSERT_EQUAL_STRING("committing", cup_update_phase_name(CUP_UPDATE_PHASE_COMMITTING));
    TEST_ASSERT_EQUAL_STRING("failed", cup_update_phase_name(CUP_UPDATE_PHASE_FAILED));
    TEST_ASSERT_EQUAL_STRING("invalid", cup_update_phase_name((CupUpdatePhase)99));

    make_staging("cup-update-begin", staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_begin(staging, "token-1", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING("cup-update-begin", journal.temporary_name);
    TEST_ASSERT_EQUAL_STRING("token-1", journal.token);
    TEST_ASSERT_EQUAL_STRING("1.2.3", journal.version);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_get_staging_path(&journal, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(staging, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_set_phase(&journal, CUP_UPDATE_PHASE_COMMITTING, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_COMMITTING, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          cup_update_journal_set_phase(&journal, CUP_UPDATE_PHASE_FAILED, 19));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
    TEST_ASSERT_EQUAL_INT(19, journal.error_code);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        cup_update_journal_set_phase(NULL, CUP_UPDATE_PHASE_FAILED, 1));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        cup_update_journal_set_phase(&journal, (CupUpdatePhase)99, 1));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        cup_update_journal_set_phase(&journal, CUP_UPDATE_PHASE_FAILED, -1));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_journal_begin(staging, "token-2", "1.2.4"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_clear());
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_journal_begin("/tmp/not-a-cup-update", "token", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_journal_begin(staging, "bad token", "1"));
}

static void assert_invalid_journal(const char *text) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status = CUP_UPDATE_JOURNAL_MISSING;

    write_journal(text);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_journal_load(&journal, &status));
}

static void test_strict_load(void) {
    static const char *invalid[] = {
        "format=2\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=0\n",
        "format=1\noperation=unknown\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=0\n",
        "format=1\noperation=cup-update\nphase=unknown\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=0\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=other\ntoken=t\nversion=1\nerror=0\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=bad token\nversion=1\nerror=0\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=\nerror=0\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=-1\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=256\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=1\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=0\nunknown=x\n",
        "format=1\nformat=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=0\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=not-a-number\n",
        "not-a-key-value\n"
    };
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;
    size_t i;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_journal_load(NULL, &status));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_journal_load(&journal, NULL));

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_invalid_journal(invalid[i]);
    }

    write_journal("format=1\noperation=cup-update\nphase=failed\n"
                  "temporary_name=cup-update-x\ntoken=t\nversion=1\nerror=19\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
}

static void assert_invalid_result(const char *text) {
    CupUpdateResult result;
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_cup_update_result_path(path, sizeof(path)));
    write_file(path, text);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, cup_update_result_load(&result));
}

static void test_persisted_result(void) {
    static const char *invalid[] = {
        "format=9\nstatus=success\nerror=0\nversion=1\n",
        "format=1\nstatus=unknown\nerror=0\nversion=1\n",
        "format=1\nstatus=success\nerror=-1\nversion=1\n",
        "format=1\nstatus=success\nerror=256\nversion=1\n",
        "format=1\nstatus=success\nerror=1\nversion=1\n",
        "format=1\nstatus=success\nerror=0\nversion=\n",
        "format=1\nstatus=success\nerror=0\nversion=1\nunknown=x\n",
        "format=1\nformat=1\nstatus=success\nerror=0\nversion=1\n",
        "format=1\nstatus=success\nerror=0\n",
        "not-a-key-value\n"
    };
    CupUpdateResult result;
    size_t i;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_update_result_load(NULL));
    cup_update_result_init(&result);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_MISSING, result.status);
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_MISSING, result.status);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_result_write(CUP_UPDATE_RESULT_MISSING, 0, "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, -1, "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, ""));

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, "2.0.0"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_SUCCESS, result.status);
    TEST_ASSERT_EQUAL_STRING("2.0.0", result.version);

    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_write(CUP_UPDATE_RESULT_FAILED, 19, "2.0.1"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_update_result_load(&result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RESULT_FAILED, result.status);
    TEST_ASSERT_EQUAL_INT(19, result.error_code);

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_invalid_result(invalid[i]);
    }
}

static void test_persistent_writes_map_replace_state(void) {
    char staging[MAX_PATH_LEN];

    make_staging("cup-update-replace-state", staging, sizeof(staging));
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_journal_begin(staging, "token", "1.0.0"));

    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          cup_update_journal_begin(staging, "token", "1.0.0"));

    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, "1.0.0"));
    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          cup_update_result_write(CUP_UPDATE_RESULT_SUCCESS, 0, "1.0.0"));
}

static void test_recover_committed(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-committed");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    write_file(marker, "ok");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_FINALIZED, result);
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_committed_ignores_staging_cleanup_failure(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-committed-cleanup-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    write_file(marker, "ok");
    write_journal("journal");
    remove_tree_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_FINALIZED, result);
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_rollback(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-rollback");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "old");
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_UNINSTALL_EXECUTABLE ? 2 : 1, executable_calls);
    TEST_ASSERT_EQUAL_INT(5, read_only_calls);
    TEST_ASSERT_EQUAL_INT(6, writable_calls);
    TEST_ASSERT_EQUAL_INT(6, replaced_path_count);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(path, replaced_paths[replaced_path_count - 1]);
}

static void test_recover_rollback_ignores_staging_cleanup_failure(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-rollback-cleanup-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");
    remove_tree_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    assert_file_text(binary, "old");
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_preserves_running_binary(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-preserve");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    write_file(binary, "old");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_PRESERVE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    assert_file_text(binary, "old");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(uninstall, sizeof(uninstall)));
    assert_file_text(uninstall, "old");
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_recover_rejects_running_binary_replacement(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-preserve-mismatch");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        cup_update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_PRESERVE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    assert_file_text(binary, "new");
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(uninstall, sizeof(uninstall)));
    assert_file_text(uninstall, "new");
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
}

static void test_recovery_rejects_invalid_state(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_FINALIZED;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        cup_update_journal_recover(NULL, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_update_journal_recover(&journal, (CupUpdateRecoveryMode)99, &result));

    strcpy(journal.temporary_name, "cup-update-invalid-marker");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    make_dir(marker);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        cup_update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
}

static void test_recovery_maps_restore_failures(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-restore-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ROLLBACK,
        cup_update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));

    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        cup_update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
}

static void test_recovery_rejects_permission_failure(void) {
    CupUpdateJournal journal;
    CupUpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];

    cup_update_journal_init(&journal);
    strcpy(journal.temporary_name, "cup-update-permission-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");
    permission_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        cup_update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_model_and_begin);
    RUN_TEST(test_strict_load);
    RUN_TEST(test_persisted_result);
    RUN_TEST(test_persistent_writes_map_replace_state);
    RUN_TEST(test_recover_committed);
    RUN_TEST(test_recover_committed_ignores_staging_cleanup_failure);
    RUN_TEST(test_recover_rollback);
    RUN_TEST(test_recover_rollback_ignores_staging_cleanup_failure);
    RUN_TEST(test_recover_preserves_running_binary);
    RUN_TEST(test_recover_rejects_running_binary_replacement);
    RUN_TEST(test_recovery_rejects_invalid_state);
    RUN_TEST(test_recovery_maps_restore_failures);
    RUN_TEST(test_recovery_rejects_permission_failure);
    return UNITY_END();
}
