/* Exercises native uninstall handoff, detach ordering and crash-evidence preservation. */

#include "uninstall_helper.h"

#include "checksum.h"
#include "exit_status.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "uninstall_journal.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static const char *root = "/home/test/.cup";
static const char *detached = "/home/test/.cup-uninstall-token";
static const char *token = "token";
static const char *running = "/work/cup";

static SystemPathKind helper_kind;
static CupError executable_path_result;
static CupError copy_result;
static CupError executable_result;
static CupError start_result;
static CupError unlink_result;
static CupError cleanup_validate_result;
static CupError handoff_result;
static CupError root_validate_result;
static CupError journal_load_result;
static UninstallJournalStatus journal_status;
static UninstallPhase root_phase;
static UninstallStage root_stage;
static int root_error;
static char journal_token[MAX_TRANSACTION_TOKEN_LEN];
static char journal_name[MAX_METADATA_LINE_LEN];
static CupError journal_set_result;
static CupError move_result;
static SystemCommitState move_state;
static CupError detached_identity_result;
static CupError remove_contents_result;
static CupError remove_journal_result;
static CupError remove_root_result;
static CupError helper_identity_result;
static CupError remove_helper_result;
static int hash_mismatch;

static int copy_calls;
static int remove_helper_calls;
static int start_calls;
static int unlink_calls;
static int cleanup_validate_calls;
static int handoff_accept_calls;
static int handoff_release_calls;
static int validate_calls;
static int journal_load_calls;
static int detaching_writes;
static int failed_writes;
static int move_calls;
static int remove_contents_calls;
static int remove_journal_calls;
static int remove_root_calls;
static int sequence;
static int cleanup_validate_sequence;
static int handoff_sequence;
static int detaching_sequence;
static int move_sequence;

static void reset_scenario(void) {
    helper_kind = SYSTEM_PATH_MISSING;
    executable_path_result = CUP_OK;
    copy_result = CUP_OK;
    executable_result = CUP_OK;
    start_result = CUP_OK;
    unlink_result = CUP_OK;
    cleanup_validate_result = CUP_OK;
    handoff_result = CUP_OK;
    root_validate_result = CUP_OK;
    journal_load_result = CUP_OK;
    journal_status = UNINSTALL_JOURNAL_LOADED;
    root_phase = UNINSTALL_PHASE_SCHEDULED;
    root_stage = UNINSTALL_STAGE_HANDOFF;
    root_error = 0;
    strcpy(journal_token, token);
    strcpy(journal_name, ".cup-uninstall-token");
    journal_set_result = CUP_OK;
    move_result = CUP_OK;
    move_state = SYSTEM_COMMIT_DURABLE;
    detached_identity_result = CUP_OK;
    remove_contents_result = CUP_OK;
    remove_journal_result = CUP_OK;
    remove_root_result = CUP_OK;
    helper_identity_result = CUP_OK;
    remove_helper_result = CUP_OK;
    hash_mismatch = 0;

    copy_calls = 0;
    remove_helper_calls = 0;
    start_calls = 0;
    unlink_calls = 0;
    cleanup_validate_calls = 0;
    handoff_accept_calls = 0;
    handoff_release_calls = 0;
    validate_calls = 0;
    journal_load_calls = 0;
    detaching_writes = 0;
    failed_writes = 0;
    move_calls = 0;
    remove_contents_calls = 0;
    remove_journal_calls = 0;
    remove_root_calls = 0;
    sequence = 0;
    cleanup_validate_sequence = 0;
    handoff_sequence = 0;
    detaching_sequence = 0;
    move_sequence = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

static SystemLock exclusive_lock(void) {
    SystemLock lock = {0};

    lock.handle = 7;
    lock.mode = SYSTEM_LOCK_EXCLUSIVE;
    lock.active = 1;
    return lock;
}

static void expected_helper_path(char *buffer, size_t size) {
#if defined(_WIN32)
    TEST_ASSERT_TRUE(snprintf(buffer, size, "/home/test/.cup-uninstall-helper-token.exe") > 0);
#else
    TEST_ASSERT_TRUE(snprintf(buffer, size, "/home/test/.cup-uninstall-helper-token") > 0);
#endif
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    char helper[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_EQUAL_STRING(helper, path);
    TEST_ASSERT_NOT_NULL(kind);
    *kind = helper_kind;
    return CUP_OK;
}

CupError system_get_executable_path(char *buffer, size_t size) {
    if (executable_path_result != CUP_OK) {
        return executable_path_result;
    }
    return size > strlen(running) ? (strcpy(buffer, running), CUP_OK) : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_copy_file(const char *source, const char *destination) {
    char helper[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_EQUAL_STRING(running, source);
    TEST_ASSERT_EQUAL_STRING(helper, destination);
    copy_calls++;
    if (copy_result == CUP_OK || copy_result == CUP_ERR_COMMIT) {
        helper_kind = SYSTEM_PATH_REGULAR_FILE;
    }
    return copy_result;
}

CupError system_set_executable(const char *path, int executable) {
    char helper[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_EQUAL_STRING(helper, path);
    TEST_ASSERT_EQUAL_INT(1, executable);
    return executable_result;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    char helper[MAX_PATH_LEN];
    char value = 'a';

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(hex);
    if (size < CHECKSUM_SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (hash_mismatch && strcmp(path, helper) == 0) {
        value = 'b';
    }
    memset(hex, value, CHECKSUM_SHA256_HEX_LENGTH);
    hex[CHECKSUM_SHA256_HEX_LENGTH] = '\0';
    return CUP_OK;
}

CupError system_start_uninstall_helper(const char *helper,
                                       const char *selected_root,
                                       const char *detached_root,
                                       const char *selected_token,
                                       SystemLock *lock) {
    char expected[MAX_PATH_LEN];

    expected_helper_path(expected, sizeof(expected));
    TEST_ASSERT_EQUAL_STRING(expected, helper);
    TEST_ASSERT_EQUAL_STRING(root, selected_root);
    TEST_ASSERT_EQUAL_STRING(detached, detached_root);
    TEST_ASSERT_EQUAL_STRING(token, selected_token);
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_TRUE(lock->active);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, lock->mode);
    start_calls++;
    if (start_result == CUP_OK) {
        lock->active = 0;
        lock->mode = SYSTEM_LOCK_SHARED;
    }
    return start_result;
}

#if defined(_WIN32)
CupError system_validate_uninstall_helper_cleanup(const char *cleanup_handle_value) {
    TEST_ASSERT_EQUAL_STRING("cleanup", cleanup_handle_value);
    cleanup_validate_calls++;
    cleanup_validate_sequence = ++sequence;
    return cleanup_validate_result;
}
#else
CupError system_unlink_running_executable(const char *path) {
    char helper[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_EQUAL_STRING(helper, path);
    unlink_calls++;
    return unlink_result;
}
#endif

CupError system_handoff_accept(SystemHandoff *handoff,
                               const char *parent_signal_value,
                               const char *authority_value) {
    TEST_ASSERT_NOT_NULL(handoff);
    TEST_ASSERT_EQUAL_STRING("wait", parent_signal_value);
    TEST_ASSERT_EQUAL_STRING("authority", authority_value);
    handoff_accept_calls++;
    handoff_sequence = ++sequence;
    if (handoff_result != CUP_OK) {
        return handoff_result;
    }
    handoff->handle = 9;
    handoff->active = 1;
    return CUP_OK;
}

void system_handoff_release(SystemHandoff *handoff) {
    if (handoff != NULL && handoff->active) {
        handoff->active = 0;
        handoff_release_calls++;
    }
}

CupError layout_validate_root_at(const char *path, SystemPathIdentity *identity) {
    TEST_ASSERT_EQUAL_STRING(root, path);
    TEST_ASSERT_NOT_NULL(identity);
    validate_calls++;
    if (root_validate_result == CUP_OK) {
        memset(identity, 0, sizeof(*identity));
        identity->valid = 1;
        identity->kind = SYSTEM_PATH_DIRECTORY;
        identity->object = 1;
    }
    return root_validate_result;
}

void uninstall_journal_init(UninstallJournal *journal) {
    TEST_ASSERT_NOT_NULL(journal);
    memset(journal, 0, sizeof(*journal));
}

CupError uninstall_journal_load_at(const char *journal_root,
                                   UninstallJournal *journal,
                                   UninstallJournalStatus *status) {
    TEST_ASSERT_NOT_NULL(journal_root);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_NOT_NULL(status);
    journal_load_calls++;
    if (journal_load_result != CUP_OK) {
        return journal_load_result;
    }
    uninstall_journal_init(journal);
    *status = journal_status;
    strcpy(journal->temporary_name, journal_name);
    strcpy(journal->token, journal_token);
    journal->file_identity.valid = 1;
    journal->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    journal->file_identity.object = 2;
    if (strcmp(journal_root, root) == 0) {
        journal->phase = root_phase;
        journal->stage = root_stage;
        journal->error_code = root_error;
    } else {
        TEST_ASSERT_EQUAL_STRING(detached, journal_root);
        journal->phase = UNINSTALL_PHASE_DETACHING;
        journal->stage = UNINSTALL_STAGE_DETACH;
        journal->error_code = 0;
    }
    return CUP_OK;
}

CupError uninstall_journal_set_at(const char *journal_root,
                                  UninstallJournal *journal,
                                  UninstallPhase phase,
                                  UninstallStage stage,
                                  int error_code) {
    TEST_ASSERT_EQUAL_STRING(root, journal_root);
    TEST_ASSERT_NOT_NULL(journal);
    if (journal_set_result != CUP_OK) {
        return journal_set_result;
    }
    sequence++;
    if (phase == UNINSTALL_PHASE_DETACHING) {
        TEST_ASSERT_EQUAL_INT(UNINSTALL_STAGE_DETACH, stage);
        TEST_ASSERT_EQUAL_INT(0, error_code);
        detaching_writes++;
        detaching_sequence = sequence;
        root_phase = phase;
        root_stage = stage;
        root_error = error_code;
    } else {
        TEST_ASSERT_EQUAL_INT(UNINSTALL_PHASE_FAILED, phase);
        TEST_ASSERT_EQUAL_INT(UNINSTALL_STAGE_DETACH, stage);
        TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, error_code);
        failed_writes++;
    }
    journal->phase = phase;
    journal->stage = stage;
    journal->error_code = error_code;
    return CUP_OK;
}

CupError system_move_path_retry(const char *source,
                                const char *destination,
                                const SystemPathIdentity *expected_identity,
                                SystemCommitState *commit_state) {
    TEST_ASSERT_EQUAL_STRING(root, source);
    TEST_ASSERT_EQUAL_STRING(detached, destination);
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    TEST_ASSERT_EQUAL_UINT64(1, expected_identity->object);
    TEST_ASSERT_NOT_NULL(commit_state);
    sequence++;
    move_sequence = sequence;
    move_calls++;
    *commit_state = move_state;
    return move_result;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    char helper[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    TEST_ASSERT_NOT_NULL(identity);
    if (strcmp(path, helper) == 0) {
        if (helper_identity_result != CUP_OK) {
            return helper_identity_result;
        }
        memset(identity, 0, sizeof(*identity));
        identity->valid = 1;
        identity->kind = SYSTEM_PATH_REGULAR_FILE;
        identity->object = 4;
        return CUP_OK;
    }
    TEST_ASSERT_EQUAL_STRING(detached, path);
    if (detached_identity_result != CUP_OK) {
        return detached_identity_result;
    }
    memset(identity, 0, sizeof(*identity));
    identity->valid = 1;
    identity->kind = SYSTEM_PATH_DIRECTORY;
    identity->object = 3;
    return CUP_OK;
}

CupError layout_build_transaction_path(char *buffer, size_t size, const char *selected_root) {
    return path_join(buffer, size, selected_root, "transaction.txt");
}

CupError system_remove_tree_contents(const char *path,
                                     const char *preserve_name,
                                     int (*cancelled)(void)) {
    TEST_ASSERT_EQUAL_STRING(detached, path);
    TEST_ASSERT_EQUAL_STRING("transaction.txt", preserve_name);
    TEST_ASSERT_NULL(cancelled);
    remove_contents_calls++;
    return remove_contents_result;
}

CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    char helper[MAX_PATH_LEN];
    char transaction[MAX_PATH_LEN];

    expected_helper_path(helper, sizeof(helper));
    if (strcmp(path, helper) == 0) {
        TEST_ASSERT_NOT_NULL(expected_identity);
        TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, expected_identity->kind);
        TEST_ASSERT_EQUAL_UINT64(4, expected_identity->object);
        remove_helper_calls++;
        if (remove_helper_result == CUP_OK) {
            helper_kind = SYSTEM_PATH_MISSING;
        }
        return remove_helper_result;
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(transaction, sizeof(transaction), detached, "transaction.txt"));
    TEST_ASSERT_EQUAL_STRING(transaction, path);
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_EQUAL_UINT64(2, expected_identity->object);
    remove_journal_calls++;
    return remove_journal_result;
}

CupError system_remove_path_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity,
                                        int (*cancelled)(void)) {
    TEST_ASSERT_EQUAL_STRING(detached, path);
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_EQUAL_UINT64(3, expected_identity->object);
    TEST_ASSERT_NULL(cancelled);
    remove_root_calls++;
    return remove_root_result;
}

static void test_start_requires_exclusive_lock(void) {
    SystemLock lock = exclusive_lock();
    SystemLock shared = exclusive_lock();

    shared.mode = SYSTEM_LOCK_SHARED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_start(NULL, detached, token, &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_start(root, NULL, token, &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_start(root, detached, "", &lock));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_start(root, detached, token, &shared));
}

static void test_start_prepares_exact_native_copy_and_consumes_lock(void) {
    SystemLock lock = exclusive_lock();

    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_helper_start(root, detached, token, &lock));
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
    TEST_ASSERT_EQUAL_INT(1, start_calls);
    TEST_ASSERT_FALSE(lock.active);
    TEST_ASSERT_EQUAL_INT(0, remove_helper_calls);

    reset_scenario();
    lock = exclusive_lock();
    copy_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_helper_start(root, detached, token, &lock));
    TEST_ASSERT_EQUAL_INT(1, start_calls);
    TEST_ASSERT_FALSE(lock.active);
}

static void test_start_failure_preserves_parent_authority_and_removes_copy(void) {
    SystemLock lock = exclusive_lock();

    hash_mismatch = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          uninstall_helper_start(root, detached, token, &lock));
    TEST_ASSERT_TRUE(lock.active);
    TEST_ASSERT_EQUAL_INT(0, start_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_helper_calls);

    reset_scenario();
    lock = exclusive_lock();
    start_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          uninstall_helper_start(root, detached, token, &lock));
    TEST_ASSERT_TRUE(lock.active);
    TEST_ASSERT_EQUAL_INT(1, start_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_helper_calls);
}

static void test_stale_helper_cleanup_is_identity_bound(void) {
    SystemLock lock = exclusive_lock();
    SystemLock shared = exclusive_lock();

    shared.mode = SYSTEM_LOCK_SHARED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_remove_stale(root, token, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_remove_stale(root, token, &shared));

    helper_kind = SYSTEM_PATH_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_OK, uninstall_helper_remove_stale(root, token, &lock));
    TEST_ASSERT_EQUAL_INT(0, remove_helper_calls);

    reset_scenario();
    lock = exclusive_lock();
    helper_kind = SYSTEM_PATH_DIRECTORY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          uninstall_helper_remove_stale(root, token, &lock));
    TEST_ASSERT_EQUAL_INT(0, remove_helper_calls);

    reset_scenario();
    lock = exclusive_lock();
    helper_kind = SYSTEM_PATH_REGULAR_FILE;
    helper_identity_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          uninstall_helper_remove_stale(root, token, &lock));
    TEST_ASSERT_EQUAL_INT(0, remove_helper_calls);

    reset_scenario();
    lock = exclusive_lock();
    helper_kind = SYSTEM_PATH_REGULAR_FILE;
    remove_helper_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          uninstall_helper_remove_stale(root, token, &lock));
    TEST_ASSERT_EQUAL_INT(1, remove_helper_calls);
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, helper_kind);
}

static CupError run_helper(void) {
#if defined(_WIN32)
    return uninstall_helper_run(root, detached, token, "wait", "authority", "cleanup");
#else
    return uninstall_helper_run(root, detached, token, "wait", "authority", NULL);
#endif
}

static void test_run_rejects_before_root_mutation(void) {
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          uninstall_helper_run(root,
                                               detached,
                                               token,
                                               "wait",
                                               "authority",
                                               NULL));
    TEST_ASSERT_EQUAL_INT(0, cleanup_validate_calls);
    TEST_ASSERT_EQUAL_INT(0, handoff_accept_calls);

    cleanup_validate_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, run_helper());
    TEST_ASSERT_EQUAL_INT(1, cleanup_validate_calls);
    TEST_ASSERT_EQUAL_INT(0, handoff_accept_calls);
    TEST_ASSERT_EQUAL_INT(0, move_calls);
#else
    unlink_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, run_helper());
    TEST_ASSERT_EQUAL_INT(0, handoff_accept_calls);
    TEST_ASSERT_EQUAL_INT(0, move_calls);
#endif

    reset_scenario();
    handoff_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, run_helper());
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(1, cleanup_validate_calls);
    TEST_ASSERT_EQUAL_INT(0, unlink_calls);
    TEST_ASSERT_TRUE(cleanup_validate_sequence > 0);
    TEST_ASSERT_TRUE(handoff_sequence > cleanup_validate_sequence);
#else
    TEST_ASSERT_EQUAL_INT(1, unlink_calls);
#endif
    TEST_ASSERT_EQUAL_INT(0, validate_calls);
    TEST_ASSERT_EQUAL_INT(0, move_calls);

    reset_scenario();
    strcpy(journal_token, "other");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, run_helper());
    TEST_ASSERT_EQUAL_INT(1, handoff_release_calls);
    TEST_ASSERT_EQUAL_INT(0, detaching_writes);
    TEST_ASSERT_EQUAL_INT(0, move_calls);
}

static void test_detaching_evidence_precedes_move(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, run_helper());
    TEST_ASSERT_EQUAL_INT(1, detaching_writes);
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_TRUE(detaching_sequence > 0);
    TEST_ASSERT_TRUE(move_sequence > detaching_sequence);
}

static void test_not_applied_move_records_failed_detach(void) {
    move_result = CUP_ERR_FILESYSTEM;
    move_state = SYSTEM_COMMIT_NOT_APPLIED;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, run_helper());
    TEST_ASSERT_EQUAL_INT(1, failed_writes);
    TEST_ASSERT_EQUAL_INT(0, remove_contents_calls);
    TEST_ASSERT_EQUAL_INT(1, handoff_release_calls);
}

static void test_uncertain_commit_preserves_detached_evidence(void) {
    move_result = CUP_ERR_COMMIT;
    move_state = SYSTEM_COMMIT_APPLIED;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, run_helper());
    TEST_ASSERT_EQUAL_INT(0, failed_writes);
    TEST_ASSERT_EQUAL_INT(0, remove_contents_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_journal_calls);
    TEST_ASSERT_EQUAL_INT(1, handoff_release_calls);
}

static void test_durable_detach_removes_payload_then_journal_then_root(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, run_helper());
    TEST_ASSERT_EQUAL_INT(2, journal_load_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_contents_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_journal_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_root_calls);
    TEST_ASSERT_EQUAL_INT(1, handoff_release_calls);
}

static void test_cleanup_failure_keeps_journal_and_root(void) {
    remove_contents_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, run_helper());
    TEST_ASSERT_EQUAL_INT(1, remove_contents_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_journal_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_root_calls);
    TEST_ASSERT_EQUAL_INT(1, handoff_release_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_start_requires_exclusive_lock);
    RUN_TEST(test_start_prepares_exact_native_copy_and_consumes_lock);
    RUN_TEST(test_start_failure_preserves_parent_authority_and_removes_copy);
    RUN_TEST(test_stale_helper_cleanup_is_identity_bound);
    RUN_TEST(test_run_rejects_before_root_mutation);
    RUN_TEST(test_detaching_evidence_precedes_move);
    RUN_TEST(test_not_applied_move_records_failed_detach);
    RUN_TEST(test_uncertain_commit_preserves_detached_evidence);
    RUN_TEST(test_durable_detach_removes_payload_then_journal_then_root);
    RUN_TEST(test_cleanup_failure_keeps_journal_and_root);
    return UNITY_END();
}
