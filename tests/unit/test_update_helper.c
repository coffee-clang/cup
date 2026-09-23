/* Tests only the detached-helper responsibilities: verified copy and authority handoff. */

#include "checksum.h"
#include "constants.h"
#include "layout.h"
#include "system.h"
#include "text.h"
#include "unity.h"
#include "update_helper.h"
#include "update_journal.h"

#include <stdio.h>
#include <string.h>

static CupError ensure_helpers_result;
static CupError copy_result;
static CupError executable_result;
static CupError handoff_accept_result;
static CupError handoff_lock_result;
static CupError layout_begin_result;
static CupError layout_validate_result;
static CupError journal_load_result;
static CupError generation_commit_result;
static int helper_exists;
static int helper_matches;
static int copy_calls;
static int executable_calls;
static int start_calls;
static int handoff_accept_calls;
static int handoff_lock_calls;
static int commit_calls;
static int lock_release_calls;
static int handoff_release_calls;
static int layout_end_calls;
static char root_path[MAX_PATH_LEN];
static char journal_temporary[MAX_METADATA_VALUE_LEN];

void setUp(void) {
    ensure_helpers_result = CUP_OK;
    copy_result = CUP_OK;
    executable_result = CUP_OK;
    handoff_accept_result = CUP_OK;
    handoff_lock_result = CUP_OK;
    layout_begin_result = CUP_OK;
    layout_validate_result = CUP_OK;
    journal_load_result = CUP_OK;
    generation_commit_result = CUP_OK;
    helper_exists = 0;
    helper_matches = 1;
    copy_calls = 0;
    executable_calls = 0;
    start_calls = 0;
    handoff_accept_calls = 0;
    handoff_lock_calls = 0;
    commit_calls = 0;
    lock_release_calls = 0;
    handoff_release_calls = 0;
    layout_end_calls = 0;
    strcpy(root_path, "/tmp/root");
    strcpy(journal_temporary, "cup-update-abc");
}
void tearDown(void) {}

CupError layout_ensure_helpers(void) { return ensure_helpers_result; }
CupError layout_get_update_helper_path(char *buffer, size_t size) {
    return text_copy(buffer, size, "/tmp/root/helpers/update-helper");
}
CupError layout_get_binary_path(char *buffer, size_t size) {
    return text_copy(buffer, size, "/tmp/root/bin/cup");
}
CupError layout_build_lock_path(char *buffer, size_t size, const char *root) {
    return text_format(buffer, size, "%s/cup.lock", root);
}
CupError layout_root_snapshot_begin_at(const char *root) {
    (void)root;
    return layout_begin_result;
}
CupError layout_get_root(char *buffer, size_t size) { return text_copy(buffer, size, root_path); }
CupError layout_root_snapshot_validate(void) { return layout_validate_result; }
void layout_root_snapshot_end(void) { layout_end_calls++; }

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    (void)path;
    if (kind == NULL) return CUP_ERR_INVALID_INPUT;
    *kind = helper_exists ? SYSTEM_PATH_REGULAR_FILE : SYSTEM_PATH_MISSING;
    return CUP_OK;
}
CupError checksum_sha256_file(const char *path, char *output, size_t output_size) {
    const char *digest;
    if (strstr(path, "update-helper") != NULL) {
        digest = helper_matches ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    } else {
        digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
    return text_copy(output, output_size, digest);
}
CupError system_copy_file(const char *source, const char *destination) {
    (void)source;
    (void)destination;
    copy_calls++;
    if (copy_result == CUP_OK || copy_result == CUP_ERR_COMMIT) {
        helper_exists = 1;
        helper_matches = 1;
    }
    return copy_result;
}
CupError system_set_executable(const char *path, int executable) {
    (void)path;
    TEST_ASSERT_EQUAL_INT(1, executable);
    executable_calls++;
    return executable_result;
}
CupError system_start_update_helper(const char *helper,
                                    const char *root,
                                    const char *token,
                                    SystemLock *lock) {
    (void)helper;
    (void)root;
    (void)token;
    TEST_ASSERT_NOT_NULL(lock);
    start_calls++;
    return CUP_OK;
}
CupError system_handoff_accept(SystemHandoff *handoff,
                               const char *parent_signal,
                               const char *authority) {
    (void)parent_signal;
    (void)authority;
    handoff_accept_calls++;
    if (handoff_accept_result == CUP_OK) handoff->active = 1;
    return handoff_accept_result;
}
CupError system_handoff_acquire_lock(SystemHandoff *handoff,
                                     SystemLock *lock,
                                     const char *path) {
    (void)handoff;
    (void)path;
    handoff_lock_calls++;
    if (handoff_lock_result == CUP_OK) lock->active = 1;
    return handoff_lock_result;
}
void system_handoff_release(SystemHandoff *handoff) {
    handoff_release_calls++;
    if (handoff != NULL) handoff->active = 0;
}
void system_lock_release(SystemLock *lock) {
    lock_release_calls++;
    if (lock != NULL) lock->active = 0;
}

int runtime_journal_token_is_valid(const char *token) {
    return token != NULL && token[0] != '\0' && strchr(token, '/') == NULL;
}

void update_journal_init(UpdateJournal *journal) {
    if (journal != NULL) memset(journal, 0, sizeof(*journal));
}
CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status) {
    if (journal_load_result != CUP_OK) return journal_load_result;
    update_journal_init(journal);
    strcpy(journal->temporary_name, journal_temporary);
    strcpy(journal->target_release_sha256,
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    journal->file_identity.valid = 1;
    journal->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    *status = CUP_UPDATE_JOURNAL_LOADED;
    return CUP_OK;
}
CupError update_generation_commit(const UpdateJournal *journal) {
    TEST_ASSERT_NOT_NULL(journal);
    commit_calls++;
    return generation_commit_result;
}

static void test_prepare_creates_and_verifies_helper(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_helper_prepare_from("/tmp/source-cup"));
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
    TEST_ASSERT_EQUAL_INT(1, executable_calls);
}

static void test_prepare_reuses_matching_helper(void) {
    helper_exists = 1;
    helper_matches = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_helper_prepare_from("/tmp/source-cup"));
    TEST_ASSERT_EQUAL_INT(0, copy_calls);
    TEST_ASSERT_EQUAL_INT(1, executable_calls);
}

static void test_prepare_tolerates_uncertain_disposable_copy_when_bytes_match(void) {
    copy_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_helper_prepare_from("/tmp/source-cup"));
    TEST_ASSERT_EQUAL_INT(1, copy_calls);
}

static void test_start_delegates_handoff(void) {
    SystemLock lock = {0};
    TEST_ASSERT_EQUAL_INT(CUP_OK, update_helper_start("/tmp/root", "u1-cup-update-abc", &lock));
    TEST_ASSERT_EQUAL_INT(1, start_calls);
}

static void test_run_requires_token_bound_to_journal_workspace(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          update_helper_run("/tmp/root", "u1-other", "signal", "authority"));
    TEST_ASSERT_EQUAL_INT(0, commit_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_run_commits_under_reacquired_authority(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_helper_run("/tmp/root",
                                            "u1-cup-update-abc",
                                            "signal",
                                            "authority"));
    TEST_ASSERT_EQUAL_INT(1, handoff_accept_calls);
    TEST_ASSERT_EQUAL_INT(1, handoff_lock_calls);
    TEST_ASSERT_EQUAL_INT(1, commit_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
    TEST_ASSERT_EQUAL_INT(1, layout_end_calls);
}

static void test_commit_failure_is_returned_without_secondary_recovery(void) {
    generation_commit_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          update_helper_run("/tmp/root",
                                            "u1-cup-update-abc",
                                            "signal",
                                            "authority"));
    TEST_ASSERT_EQUAL_INT(1, commit_calls);
    /* No journal mutation/recovery API exists in this owner; evidence is left untouched. */
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_prepare_creates_and_verifies_helper);
    RUN_TEST(test_prepare_reuses_matching_helper);
    RUN_TEST(test_prepare_tolerates_uncertain_disposable_copy_when_bytes_match);
    RUN_TEST(test_start_delegates_handoff);
    RUN_TEST(test_run_requires_token_bound_to_journal_workspace);
    RUN_TEST(test_run_commits_under_reacquired_authority);
    RUN_TEST(test_commit_failure_is_returned_without_secondary_recovery);
    return UNITY_END();
}
