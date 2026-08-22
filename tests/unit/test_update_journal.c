/* Exercises the strict deferred cup update journal and recovery lifecycle. */

#include "assets.h"
#include "update_journal.h"
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
static CupError copy_result;
static int copy_calls;
static int copy_fail_call;
static char copied_paths[8][MAX_PATH_LEN];
static CupError permission_result;
static CupError remove_file_result;
static CupError sync_parent_result;
static CupError remove_tree_result;
static int assets_valid;
static CupError assets_inspect_result;
static int remove_tree_calls;
static int executable_calls;
static int read_only_calls;
static int writable_calls;
static char last_executable_path[MAX_PATH_LEN];
static char replaced_paths[8][MAX_PATH_LEN];
static int replaced_path_count;

/* Fixture lifecycle and local construction helpers. */

static CupError clear_runtime_journal(void) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    CupError err;

    err = update_journal_load(&journal, &status);
    if (err == CUP_OK && status == CUP_UPDATE_JOURNAL_LOADED) {
        err = runtime_journal_clear_if_identity(&journal.file_identity);
    }
    return err;
}


static CupError begin_update_journal_for_test(const char *temporary_path,
                                                  const char *token,
                                                  const char *version) {
    UpdateJournal created;

    return update_journal_begin(temporary_path, token, version, &created);
}

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
    copy_result = CUP_OK;
    copy_calls = 0;
    copy_fail_call = 0;
    memset(copied_paths, 0, sizeof(copied_paths));
    permission_result = CUP_OK;
    remove_file_result = CUP_OK;
    sync_parent_result = CUP_OK;
    remove_tree_result = CUP_OK;
    assets_valid = 1;
    assets_inspect_result = CUP_OK;
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

int interrupt_requested(void) {
    return 0;
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

CupError layout_get_binary_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/" CUP_BINARY_FILENAME);
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

CupError system_create_file_exclusive(const char *path, FILE **file) {
    if (path == NULL || file == NULL || test_access_exists(path)) {
        return CUP_ERR_FILESYSTEM;
    }
    *file = fopen(path, "wb");
    return *file != NULL ? CUP_OK : CUP_ERR_FILESYSTEM;
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

CupError system_replace_file_if_identity(
    const char *source,
    const char *destination,
    const SystemPathIdentity *expected_identity,
    SystemCommitState *state) {
    (void)expected_identity;
    return system_replace_file(source, destination, state);
}

CupError system_copy_file(const char *source, const char *destination) {
    FILE *input;
    FILE *output;
    char buffer[256];
    size_t length;

    copy_calls++;
    if (copy_calls <= (int)(sizeof(copied_paths) / sizeof(copied_paths[0]))) {
        TEST_ASSERT_TRUE(strlen(destination) < sizeof(copied_paths[0]));
        strcpy(copied_paths[copy_calls - 1], destination);
    }
    if (copy_result != CUP_OK || (copy_fail_call != 0 && copy_calls == copy_fail_call)) {
        return copy_result != CUP_OK ? copy_result : CUP_ERR_FILESYSTEM;
    }
    input = fopen(source, "rb");
    if (input == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    output = fopen(destination, "wb");
    if (output == NULL) {
        fclose(input);
        return CUP_ERR_FILESYSTEM;
    }
    while ((length = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        if (fwrite(buffer, 1, length, output) != length) {
            fclose(input);
            fclose(output);
            return CUP_ERR_FILESYSTEM;
        }
    }
    if (ferror(input) || fclose(input) != 0 || fclose(output) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *state) {
    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (replace_result != CUP_OK) {
        *state = replace_state;
        return replace_result;
    }
    if (test_access_exists(destination)) {
        *state = SYSTEM_COMMIT_NOT_APPLIED;
        return CUP_ERR_FILESYSTEM;
    }
    if (rename(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    TestPlatformStat status;

    if (path == NULL || identity == NULL || test_stat_path(path, &status) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    memset(identity, 0, sizeof(*identity));
    identity->kind = test_stat_is_regular(&status) ? SYSTEM_PATH_REGULAR_FILE
                                                   : SYSTEM_PATH_OTHER;
    identity->valid = 1;
    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    *exists = test_access_exists(path);
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    if (remove_file_result != CUP_OK) {
        return remove_file_result;
    }
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}


CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    (void)expected_identity;
    return system_remove_file(path);
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    return sync_parent_result;
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

CupError assets_inspect(AssetsInspection *inspection) {
    memset(inspection, 0, sizeof(*inspection));
    return assets_inspect_result;
}

int assets_installed_is_valid(const AssetsInspection *inspection) {
    (void)inspection;
    return assets_valid;
}

static void make_staging(const char *name, char *path, size_t size) {
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, size, staging, name));
    make_dir(path);
}

static void write_journal_bytes(const void *contents, size_t size) {
    char path[MAX_PATH_LEN];
    FILE *file;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(contents, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_journal(const char *text) {
    write_journal_bytes(text, strlen(text));
}

static void create_destination_files(void) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
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

static void create_committed_generation(UpdateJournal *journal,
                                        const char *staging) {
    char binary[MAX_PATH_LEN];

    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    strcpy(journal->version, "1.2.3");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_write_generation_marker(staging, journal->version, binary));
}

static void create_backups(const char *staging) {
    const char *names[] = {CUP_UPDATE_BINARY_OLD,
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

static void create_absent_markers(const char *staging) {
    const char *names[] = {CUP_UPDATE_BINARY_ABSENT,
                           CUP_UPDATE_PLATFORM_CHECKSUMS_ABSENT,
                           CUP_UPDATE_PACKAGES_ABSENT,
                           CUP_UPDATE_INSTALL_POLICY_ABSENT,
                           CUP_UPDATE_COMMON_CHECKSUMS_ABSENT};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[MAX_PATH_LEN];
        TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), staging, names[i]));
        write_file(path, "absent\n");
    }
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void set_journal_identity(UpdateJournal *journal) {
    TEST_ASSERT_NOT_NULL(journal);
    journal->file_identity.volume = 0;
    journal->file_identity.object = 0;
    journal->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    journal->file_identity.valid = 1;
}

static void mark_commit_started(UpdateJournal *journal) {
    TEST_ASSERT_NOT_NULL(journal);
    journal->phase = CUP_UPDATE_PHASE_COMMITTING;
}

static void test_model_and_begin(void) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    update_journal_init(NULL);
    update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_NONE, journal.recovery);
    TEST_ASSERT_EQUAL_STRING("scheduled", update_phase_name(CUP_UPDATE_PHASE_SCHEDULED));
    TEST_ASSERT_EQUAL_STRING("committing", update_phase_name(CUP_UPDATE_PHASE_COMMITTING));
    TEST_ASSERT_EQUAL_STRING("failed", update_phase_name(CUP_UPDATE_PHASE_FAILED));
    TEST_ASSERT_EQUAL_STRING("invalid", update_phase_name((UpdatePhase)99));
    TEST_ASSERT_EQUAL_STRING("none", update_failure_recovery_name(CUP_UPDATE_FAILURE_NONE));
    TEST_ASSERT_EQUAL_STRING(
        "pending", update_failure_recovery_name(CUP_UPDATE_FAILURE_PENDING));
    TEST_ASSERT_EQUAL_STRING("rolled-back",
                             update_failure_recovery_name(CUP_UPDATE_FAILURE_ROLLED_BACK));
    TEST_ASSERT_EQUAL_STRING(
        "invalid", update_failure_recovery_name((UpdateFailureRecovery)99));

    make_staging("cup-update-begin.tmp", staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        begin_update_journal_for_test(staging, "u1234-cup-update-begin.tmp", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING("cup-update-begin.tmp", journal.temporary_name);
    TEST_ASSERT_EQUAL_STRING("u1234-cup-update-begin.tmp", journal.token);
    TEST_ASSERT_EQUAL_STRING("1.2.3", journal.version);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_SCHEDULED, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_NONE, journal.recovery);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_journal_get_staging_path(&journal, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(staging, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_journal_set_phase(&journal, CUP_UPDATE_PHASE_COMMITTING, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_journal_set_phase(&journal, CUP_UPDATE_PHASE_FAILED, 19));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_PENDING, journal.recovery);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_PENDING, journal.recovery);
    TEST_ASSERT_EQUAL_INT(19, journal.error_code);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_set_phase(NULL, CUP_UPDATE_PHASE_FAILED, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_set_phase(&journal, (UpdatePhase)99, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_set_phase(&journal, CUP_UPDATE_PHASE_FAILED, 0));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        begin_update_journal_for_test(staging, "u1234-cup-update-begin.tmp", "1.2.4"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_MISSING, status);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        begin_update_journal_for_test("/tmp/not-a-cup-update", "token", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(NULL, "token", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test("", "token", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(staging, NULL, "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(staging, "", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(
                              staging, "u1234-cup-update-other.tmp", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(staging, "bad token", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(
                              staging, "u1234-cup-update-begin.tmp", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(
                              staging, "u1234-cup-update-begin.tmp", ""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          begin_update_journal_for_test(staging,
                                                   "u1234-cup-update-begin.tmp",
                                                   "01.2.3"));
}


static void test_token_character_domain(void) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    char staging[MAX_PATH_LEN];

    make_staging("cup-update-token_chars.tmp", staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        begin_update_journal_for_test(
            staging, "A_0-cup-update-token_chars.tmp", "0.0.0"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING("A_0-cup-update-token_chars.tmp", journal.token);
    TEST_ASSERT_EQUAL_STRING("0.0.0", journal.version);
    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
}

static void test_public_path_contracts(void) {
    UpdateJournal journal;
    char buffer[MAX_PATH_LEN];

    update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_get_staging_path(NULL, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_get_staging_path(&journal, NULL, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_get_staging_path(&journal, buffer, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_get_staging_path(
                              &journal, buffer, sizeof(buffer)));
}

static void assert_invalid_journal(const char *text) {
    UpdateJournal journal;
    UpdateJournalStatus status = CUP_UPDATE_JOURNAL_MISSING;

    write_journal(text);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, update_journal_load(&journal, &status));
}

static void test_strict_load(void) {
    static const char *invalid[] = {
        "format=2\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=unknown\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=unknown\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=other\ntoken=u-other\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=bad token\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-y\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=01.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1..3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3.4\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1000000.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=1\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=text\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=-1\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=256\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=999999999999999999999\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=failed\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=19\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=failed\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=pending\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=invalid\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=none\nunknown=x\n",
        "format=1\nformat=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-\ntoken=u-cup-update-\nversion=1.2.3\nerror=0\nrecovery=none\n",
        "format=1\noperation=cup-update\nphase=scheduled\n"
        "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\nerror=0\n",
        "not-a-key-value\n"
    };
    UpdateJournal journal;
    UpdateJournalStatus status;
    size_t i;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, update_journal_load(NULL, &status));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, update_journal_load(&journal, NULL));

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_invalid_journal(invalid[i]);
    }


    {
        static const unsigned char hidden_nul[] =
            "format=1\noperation=cup-update\nphase=scheduled\n"
            "temporary_name=cup-update-x\ntoken=u-cup-update-x\n"
            "version=1.2.3\nerror=0\nrecovery=none\0\n";
        write_journal_bytes(hidden_nul, sizeof(hidden_nul) - 1);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_TRANSACTION, update_journal_load(&journal, &status));
    }

    assert_invalid_journal("format=1\noperation=cup-update\nphase=scheduled\n"
                           "temporary_name=cup-update-x\ntoken=u-cup-update-x\n"
                           "version=1.2.3\nerror=0\nrecovery=none");
    assert_invalid_journal("format=1\r\noperation=cup-update\nphase=scheduled\n"
                           "temporary_name=cup-update-x\ntoken=u-cup-update-x\n"
                           "version=1.2.3\nerror=0\nrecovery=none\n");

    write_journal("format=1\noperation=cup-update\nphase=failed\n"
                  "temporary_name=cup-update-x\ntoken=u-cup-update-x\nversion=1.2.3\n"
                  "error=19\nrecovery=pending\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_PHASE_FAILED, journal.phase);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_PENDING, journal.recovery);
}

static void test_persistent_writes_map_replace_state(void) {
    char staging[MAX_PATH_LEN];

    make_staging("cup-update-replace-state", staging, sizeof(staging));
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        begin_update_journal_for_test(staging,
                                 "u-cup-update-replace-state",
                                 "1.0.0"));

    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        begin_update_journal_for_test(staging,
                                 "u-cup-update-replace-state",
                                 "1.0.0"));
}

static void test_generation_marker_preserves_uncertain_commit(void) {
    UpdateJournal journal;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];

    update_journal_init(&journal);
    strcpy(journal.version, "1.2.3");
    make_staging("cup-update-marker-sync-failure", staging, sizeof(staging));
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    sync_parent_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        update_write_generation_marker(staging, journal.version, binary));
    TEST_ASSERT_TRUE(test_access_exists(marker));
}

static void test_recover_scheduled_discards_staging_without_restore(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];

    update_journal_init(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-scheduled");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    write_file(binary, "unchanged");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    assert_file_text(binary, "unchanged");
    TEST_ASSERT_EQUAL_INT(0, copy_calls);
    TEST_ASSERT_EQUAL_INT(0, replaced_path_count);
    TEST_ASSERT_FALSE(test_access_exists(staging));
}

static void test_recover_committed(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-committed");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    create_committed_generation(&journal, staging);
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_FINALIZED, result);
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_committed_ignores_staging_cleanup_failure(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-committed-cleanup-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    create_committed_generation(&journal, staging);
    write_journal("journal");
    remove_tree_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_FINALIZED, result);
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_rollback(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-rollback");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    assert_file_text(path, "old");
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(1, executable_calls);
    TEST_ASSERT_EQUAL_INT(4, read_only_calls);
    TEST_ASSERT_EQUAL_INT(5, writable_calls);
    TEST_ASSERT_EQUAL_INT(0, replaced_path_count);
    TEST_ASSERT_EQUAL_INT(5, copy_calls);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(path, copied_paths[copy_calls - 1]);
}

static void test_recover_initial_install_rollback(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char paths[5][MAX_PATH_LEN];
    size_t i;

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-initial-rollback");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_absent_markers(staging);
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(paths[0], sizeof(paths[0])));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_get_platform_checksums_path(paths[1], sizeof(paths[1])));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_get_package_catalog_path(paths[2], sizeof(paths[2])));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_get_install_policy_path(paths[3], sizeof(paths[3])));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_get_common_checksums_path(paths[4], sizeof(paths[4])));

    /* A failed initial install may have created only part of the final asset set. */
    TEST_ASSERT_EQUAL_INT(0, test_unlink(paths[4]));
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        TEST_ASSERT_FALSE(test_access_exists(paths[i]));
    }
    TEST_ASSERT_FALSE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(4, writable_calls);
}

static void test_recover_rollback_ignores_staging_cleanup_failure(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-rollback-cleanup-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");
    remove_tree_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    assert_file_text(binary, "old");
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}

static void test_recover_preserves_running_binary(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-preserve");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    write_file(binary, "old");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_PRESERVE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    assert_file_text(binary, "old");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_get_platform_checksums_path(platform_checksums, sizeof(platform_checksums)));
    assert_file_text(platform_checksums, "old");
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_recover_rejects_running_binary_replacement(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    strcpy(journal.temporary_name, "cup-update-preserve-mismatch");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_PRESERVE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    assert_file_text(binary, "new");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_get_platform_checksums_path(platform_checksums, sizeof(platform_checksums)));
    assert_file_text(platform_checksums, "new");
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
}

static void test_failed_recovery_is_acknowledged_by_repair(void) {
    UpdateJournal journal;
    UpdateJournalStatus status;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    make_staging("cup-update-failed", staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        begin_update_journal_for_test(staging, "u-cup-update-failed", "2.0.0"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_journal_set_phase(
                              &journal, CUP_UPDATE_PHASE_FAILED, 19));
    create_backups(staging);
    create_destination_files();
    remove_tree_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_FAILURE_ROLLED_BACK, journal.recovery);
    TEST_ASSERT_TRUE(test_access_exists(staging));

    remove_tree_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ACKNOWLEDGED, result);
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));
}


static void test_stale_committed_marker_rolls_back(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-stale-marker");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_committed_generation(&journal, staging);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(binary, sizeof(binary)));
    write_file(binary, "restored-old-generation");
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    assert_file_text(binary, "old");
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_unreadable_committed_generation_is_preserved(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char catalog[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-unreadable-generation");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_committed_generation(&journal, staging);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(catalog, sizeof(catalog)));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(catalog));
    write_journal("journal");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_TRUE(test_access_exists(marker));
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
    TEST_ASSERT_EQUAL_INT(0, replaced_path_count);
}

static void test_uninspectable_committed_generation_is_preserved(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-uninspectable-generation");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_committed_generation(&journal, staging);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    write_journal("journal");
    assets_inspect_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_TRUE(test_access_exists(marker));
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal_path, sizeof(journal_path)));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
    TEST_ASSERT_EQUAL_INT(0, replaced_path_count);
}

static void test_malformed_committed_marker_is_preserved(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    strcpy(journal.version, "1.2.3");
    strcpy(journal.temporary_name, "cup-update-malformed-marker");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    write_file(marker, "not-a-generation-marker\n");

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_TRUE(test_access_exists(marker));
}

static void test_acknowledgement_preserves_staging_until_generation_is_valid(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];

    update_journal_init(&journal);
    set_journal_identity(&journal);
    strcpy(journal.version, "2.0.0");
    strcpy(journal.temporary_name, "cup-update-invalid-ack");
    journal.phase = CUP_UPDATE_PHASE_FAILED;
    journal.recovery = CUP_UPDATE_FAILURE_ROLLED_BACK;
    journal.error_code = 19;
    make_staging(journal.temporary_name, staging, sizeof(staging));
    assets_valid = 0;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, update_journal_recover(
            &journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
}

static void test_recovery_rejects_invalid_state(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_FINALIZED;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];

    update_journal_init(&journal);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        update_journal_recover(NULL, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          update_journal_recover(&journal, (UpdateRecoveryMode)99, &result));

    mark_commit_started(&journal);
    strcpy(journal.temporary_name, "cup-update-invalid-marker");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED));
    make_dir(marker);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
}

static void test_recovery_maps_restore_failures(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    strcpy(journal.temporary_name, "cup-update-restore-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    copy_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ROLLBACK,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));

    copy_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
}

static void test_interrupted_rollback_can_retry_from_intact_backups(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    char destination[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    set_journal_identity(&journal);
    strcpy(journal.temporary_name, "cup-update-retry-rollback");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");

    copy_fail_call = 3;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ROLLBACK,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(backup, sizeof(backup), staging, CUP_UPDATE_PACKAGES_OLD));
    TEST_ASSERT_TRUE(test_access_exists(backup));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(destination, sizeof(destination)));
    assert_file_text(destination, "new");

    copy_fail_call = 0;
    copy_calls = 0;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_ROLLED_BACK, result);
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_recovery_rejects_permission_failure(void) {
    UpdateJournal journal;
    UpdateRecoveryResult result = CUP_UPDATE_RECOVERY_NONE;
    char staging[MAX_PATH_LEN];

    update_journal_init(&journal);
    mark_commit_started(&journal);
    strcpy(journal.temporary_name, "cup-update-permission-failure");
    make_staging(journal.temporary_name, staging, sizeof(staging));
    create_backups(staging);
    create_destination_files();
    write_journal("journal");
    permission_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        update_journal_recover(&journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &result));
    TEST_ASSERT_EQUAL_INT(CUP_UPDATE_RECOVERY_NONE, result);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_model_and_begin);
    RUN_TEST(test_token_character_domain);
    RUN_TEST(test_public_path_contracts);
    RUN_TEST(test_strict_load);
    RUN_TEST(test_persistent_writes_map_replace_state);
    RUN_TEST(test_generation_marker_preserves_uncertain_commit);
    RUN_TEST(test_recover_scheduled_discards_staging_without_restore);
    RUN_TEST(test_recover_committed);
    RUN_TEST(test_recover_committed_ignores_staging_cleanup_failure);
    RUN_TEST(test_recover_rollback);
    RUN_TEST(test_recover_initial_install_rollback);
    RUN_TEST(test_recover_rollback_ignores_staging_cleanup_failure);
    RUN_TEST(test_recover_preserves_running_binary);
    RUN_TEST(test_recover_rejects_running_binary_replacement);
    RUN_TEST(test_failed_recovery_is_acknowledged_by_repair);
    RUN_TEST(test_stale_committed_marker_rolls_back);
    RUN_TEST(test_unreadable_committed_generation_is_preserved);
    RUN_TEST(test_uninspectable_committed_generation_is_preserved);
    RUN_TEST(test_malformed_committed_marker_is_preserved);
    RUN_TEST(test_acknowledgement_preserves_staging_until_generation_is_valid);
    RUN_TEST(test_recovery_rejects_invalid_state);
    RUN_TEST(test_recovery_maps_restore_failures);
    RUN_TEST(test_interrupted_rollback_can_retry_from_intact_backups);
    RUN_TEST(test_recovery_rejects_permission_failure);
    return UNITY_END();
}
