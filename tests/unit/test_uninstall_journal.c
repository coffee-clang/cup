/* Exercises the strict uninstall journal stored in the shared transaction.txt file. */

#include "layout.h"
#include "exit_status.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "uninstall_journal.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static char root[MAX_PATH_LEN];
static CupError remove_result;
static CupError sync_result;
static CupError sync_file_result;
static CupError replace_result;
static SystemCommitState replace_state;

static CupError clear_runtime_journal(void) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    CupError err;

    err = uninstall_journal_load(&journal, &status);
    if (err == CUP_OK && status == UNINSTALL_JOURNAL_LOADED) {
        err = runtime_journal_clear_if_identity(&journal.file_identity);
    }
    return err;
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

void setUp(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];

    TEST_ASSERT_NOT_NULL(
        test_make_temp_directory(template_path, sizeof(template_path), "uninstall-journal-unit"));
    TEST_ASSERT_TRUE(strlen(template_path) < sizeof(root));
    strcpy(root, template_path);
    remove_result = CUP_OK;
    sync_result = CUP_OK;
    sync_file_result = CUP_OK;
    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
}

void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(root));
}

CupError layout_get_root(char *buffer, size_t size) {
    int written = snprintf(buffer, size, "%s", root);

    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "transaction.txt");
}

CupError layout_build_transaction_path(char *buffer, size_t size, const char *selected_root) {
    return path_join(buffer, size, selected_root, "transaction.txt");
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t size, FILE **file) {
    return test_create_temp_file(directory, prefix, path, size, file) == 0
               ? CUP_OK
               : CUP_ERR_TEMPORARY;
}

CupError system_sync_file(FILE *file) {
    if (sync_file_result != CUP_OK) {
        return sync_file_result;
    }
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = replace_state;
    if (replace_result != CUP_OK) {
        return replace_result;
    }
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (test_replace_file(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = SYSTEM_COMMIT_DURABLE;
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

CupError system_replace_file_if_identity(const char *source,
                                         const char *destination,
                                         const SystemPathIdentity *expected_identity,
                                         SystemCommitState *state) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    return system_move_path(source, destination, state);
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
    if (remove_result != CUP_OK) {
        return remove_result;
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
    return sync_result;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TestPlatformStat status;

    if (test_stat_path(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (test_stat_is_directory(&status)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else if (test_stat_is_regular(&status)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

static void assert_invalid(const char *text) {
    UninstallJournal journal;
    UninstallJournalStatus status = UNINSTALL_JOURNAL_MISSING;

    write_journal(text);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, uninstall_journal_load(&journal, &status));
}

static void test_begin_load_and_detached_path(void) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    RuntimeJournalKind kind;
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(parent, sizeof(parent), root));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(temporary, sizeof(temporary), parent, ".cup-uninstall-token"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_begin(temporary, "token"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_UNINSTALL, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(UNINSTALL_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING(".cup-uninstall-token", journal.temporary_name);
    TEST_ASSERT_EQUAL_STRING("token", journal.token);
    TEST_ASSERT_EQUAL_INT(UNINSTALL_PHASE_SCHEDULED, journal.phase);
    TEST_ASSERT_EQUAL_INT(UNINSTALL_STAGE_HANDOFF, journal.stage);
    TEST_ASSERT_EQUAL_INT(0, journal.error_code);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, uninstall_journal_begin(temporary, "token"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_begin(NULL, "token"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_begin("", "token"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_begin(temporary, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_begin(temporary, ""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_journal_begin(temporary, "bad token"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_journal_begin(temporary, "other"));
}


static void test_token_character_domain(void) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(parent, sizeof(parent), root));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(temporary,
                                    sizeof(temporary),
                                    parent,
                                    ".cup-uninstall-123-456-0.tmp"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_begin(temporary, "123-456-0.tmp"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(UNINSTALL_JOURNAL_LOADED, status);
    TEST_ASSERT_EQUAL_STRING("123-456-0.tmp", journal.token);
    TEST_ASSERT_EQUAL_STRING(".cup-uninstall-123-456-0.tmp", journal.temporary_name);
    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
}

static void test_public_argument_contracts(void) {
    UninstallJournal journal;

    uninstall_journal_init(NULL);
    uninstall_journal_init(&journal);
    TEST_ASSERT_EQUAL_STRING("invalid", uninstall_phase_name((UninstallPhase)99));
    TEST_ASSERT_EQUAL_STRING("invalid", uninstall_stage_name((UninstallStage)99));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_recover(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_recover(&journal));
}

static void test_strict_load(void) {
    static const char *invalid[] = {
        /* Detached names use one canonical dash separator. */
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall.token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=2\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=other\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=unknown\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-other\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=bad token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=unknown\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=1\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=unknown\nerror=0\n",
        "format=1\noperation=uninstall\nphase=detaching\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=detaching\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=detach\nerror=1\n",
        "format=1\noperation=uninstall\nphase=failed\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=unknown\nerror=0\n",
        "format=1\noperation=uninstall\nphase=failed\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=unknown\nerror=1\nunknown=x\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=text\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=-1\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=256\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\n"
        "token=token\n"
        "stage=handoff\n"
        "error=999999999999999999999\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token!\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\n",
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\nerror=0\n",
        "not-a-key-value\n"
    };
    UninstallJournal journal;
    UninstallJournalStatus status;
    size_t i;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_load(NULL, &status));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, uninstall_journal_load(&journal, NULL));
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_invalid(invalid[i]);
    }


    {
        static const unsigned char hidden_nul[] =
            "format=1\noperation=uninstall\nphase=scheduled\n"
            "temporary_name=.cup-uninstall-token\ntoken=token\n"
            "stage=handoff\nerror=0\0\n";
        write_journal_bytes(hidden_nul, sizeof(hidden_nul) - 1);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_TRANSACTION, uninstall_journal_load(&journal, &status));
    }

    assert_invalid("format=1\noperation=uninstall\nphase=scheduled\n"
                   "temporary_name=.cup-uninstall-token\ntoken=token\n"
                   "stage=handoff\nerror=0");
    assert_invalid("format=1\r\noperation=uninstall\nphase=scheduled\n"
                   "temporary_name=.cup-uninstall-token\ntoken=token\n"
                   "stage=handoff\nerror=0\n");

    write_journal("format=1\noperation=uninstall\nphase=detaching\n"
                  "temporary_name=.cup-uninstall-token\ntoken=token\n"
                  "stage=detach\nerror=0\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(UNINSTALL_PHASE_DETACHING, journal.phase);
    TEST_ASSERT_EQUAL_INT(UNINSTALL_STAGE_DETACH, journal.stage);
    TEST_ASSERT_EQUAL_INT(0, journal.error_code);

    write_journal("format=1\noperation=uninstall\nphase=failed\n"
                  "temporary_name=.cup-uninstall-token\ntoken=token\n"
                  "stage=detach\nerror=6\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(UNINSTALL_PHASE_FAILED, journal.phase);
    TEST_ASSERT_EQUAL_INT(UNINSTALL_STAGE_DETACH, journal.stage);
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, journal.error_code);

    assert_invalid("format=1\noperation=uninstall\nphase=failed\n"
                   "temporary_name=.cup-uninstall-token\ntoken=token\n"
                   "stage=handoff\nerror=6\n");
    assert_invalid("format=1\noperation=uninstall\nphase=failed\n"
                   "temporary_name=.cup-uninstall-token\ntoken=token\n"
                   "stage=detach\nerror=19\n");
}

static void test_persistent_write_failures(void) {
    char parent[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(parent, sizeof(parent), root));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(temporary, sizeof(temporary), parent, ".cup-uninstall-failure"));

    sync_file_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          uninstall_journal_begin(temporary, "failure"));

    sync_file_result = CUP_OK;
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          uninstall_journal_begin(temporary, "failure"));

    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          uninstall_journal_begin(temporary, "failure"));
}

static void test_recover_failed_uninstall(void) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    char path[MAX_PATH_LEN];

    write_journal("format=1\noperation=uninstall\nphase=failed\n"
                  "temporary_name=.cup-uninstall-token\ntoken=token\n"
                  "stage=detach\nerror=6\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_recover(&journal));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    TEST_ASSERT_FALSE(test_access_exists(path));

    write_journal("format=1\noperation=uninstall\nphase=failed\n"
                  "temporary_name=.cup-uninstall-token\ntoken=token\n"
                  "stage=detach\nerror=6\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    remove_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, uninstall_journal_recover(&journal));
    TEST_ASSERT_TRUE(test_access_exists(path));
}

static void test_recovery_preserves_existing_detached_root(void) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    char parent[MAX_PATH_LEN];
    char detached[MAX_PATH_LEN];

    write_journal("format=1\noperation=uninstall\nphase=failed\n"
                  "temporary_name=.cup-uninstall-token\ntoken=token\n"
                  "stage=detach\nerror=6\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(parent, sizeof(parent), root));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, path_join(detached, sizeof(detached), parent, ".cup-uninstall-token"));
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(detached, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          uninstall_journal_recover(&journal));
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(detached));
}

static void test_recover_stale_pre_detach_phases(void) {
    static const char *journals[] = {
        "format=1\noperation=uninstall\nphase=scheduled\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=handoff\nerror=0\n",
        "format=1\noperation=uninstall\nphase=detaching\n"
        "temporary_name=.cup-uninstall-token\ntoken=token\nstage=detach\nerror=0\n"};
    size_t i;

    for (i = 0; i < sizeof(journals) / sizeof(journals[0]); ++i) {
        UninstallJournal journal;
        UninstallJournalStatus status;
        char path[MAX_PATH_LEN];

        write_journal(journals[i]);
        TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_load(&journal, &status));
        TEST_ASSERT_EQUAL_INT(UNINSTALL_JOURNAL_LOADED, status);
        TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_journal_recover(&journal));
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
        TEST_ASSERT_FALSE(test_access_exists(path));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_load_and_detached_path);
    RUN_TEST(test_token_character_domain);
    RUN_TEST(test_public_argument_contracts);
    RUN_TEST(test_strict_load);
    RUN_TEST(test_persistent_write_failures);
    RUN_TEST(test_recover_failed_uninstall);
    RUN_TEST(test_recovery_preserves_existing_detached_root);
    RUN_TEST(test_recover_stale_pre_detach_phases);
    return UNITY_END();
}
