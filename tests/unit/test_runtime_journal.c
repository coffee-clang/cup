/* Exercises classification of the shared physical runtime journal. */

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

static char journal_root[CUP_TEST_TEMP_PATH_SIZE];
static char journal_path[CUP_TEST_TEMP_PATH_SIZE];
static CupError path_result;
static CupError remove_identity_result;
static CupError sync_result;
static CupError publish_result;
static SystemCommitState publish_state;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

void setUp(void) {
    int written;

    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        journal_root, sizeof(journal_root), "cup-runtime-journal-test"));
    written = snprintf(journal_path, sizeof(journal_path), "%s/transaction.txt", journal_root);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(journal_path));
    path_result = CUP_OK;
    remove_identity_result = CUP_OK;
    sync_result = CUP_OK;
    publish_result = CUP_OK;
    publish_state = SYSTEM_COMMIT_DURABLE;
}

void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(journal_root));
    journal_root[0] = '\0';
    journal_path[0] = '\0';
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_transaction_path(char *buffer, size_t size) {
    if (path_result != CUP_OK) {
        return path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", journal_path), size);
}

CupError system_create_temp_file(const char *directory,
                                 const char *prefix,
                                 char *path,
                                 size_t path_size,
                                 FILE **file) {
    return test_create_temp_file(directory, prefix, path, path_size, file) == 0
               ? CUP_OK
               : CUP_ERR_TEMPORARY;
}

CupError system_sync_file(FILE *file) {
    return file != NULL && fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    if (path == NULL || identity == NULL || !test_access_exists(path)) {
        return CUP_ERR_FILESYSTEM;
    }
    identity->volume = 1;
    identity->object = 1;
    identity->kind = SYSTEM_PATH_REGULAR_FILE;
    identity->valid = 1;
    return CUP_OK;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *state) {
    TEST_ASSERT_NOT_NULL(state);
    *state = publish_state;
    if (publish_result != CUP_OK) {
        if (publish_state == SYSTEM_COMMIT_APPLIED && !test_access_exists(destination)) {
            TEST_ASSERT_EQUAL_INT(0, rename(source, destination));
        }
        return publish_result;
    }
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (test_access_exists(destination) || rename(source, destination) != 0) {
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
    TEST_ASSERT_NOT_NULL(state);
    *state = publish_state;
    if (publish_result != CUP_OK) {
        if (publish_state == SYSTEM_COMMIT_APPLIED) {
            TEST_ASSERT_EQUAL_INT(0, test_replace_file(source, destination));
        }
        return publish_result;
    }
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (test_replace_file(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}


CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    if (remove_identity_result != CUP_OK) {
        return remove_identity_result;
    }
    return system_remove_file(path);
}

CupError system_sync_parent_directory(const char *path) {
    return path == NULL ? CUP_ERR_INVALID_INPUT : sync_result;
}

static void write_journal_bytes(const void *contents, size_t size) {
    FILE *file = fopen(journal_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(contents, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_journal(const char *contents) {
    write_journal_bytes(contents, strlen(contents));
}

static CupError accept_journal_field(const char *key, const char *value, void *userdata);

static void clear_current_journal(void) {
    SystemPathIdentity identity;
    int missing;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));
    if (!missing) {
        TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_clear_if_identity(&identity));
    }
}

static CupError accept_journal_field(const char *key, const char *value, void *userdata) {
    (void)key;
    (void)value;
    (void)userdata;
    return CUP_OK;
}

static CupError write_test_journal(FILE *file, const void *value) {
    const char *operation = value;

    return operation != NULL &&
                   fprintf(file, "format=1\noperation=%s\ntemporary_name=x\n", operation) > 0
               ? CUP_OK
               : CUP_ERR_TRANSACTION;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_detects_owners(void) {
    RuntimeJournalKind kind;

    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_require_none());

    write_journal("format=1\noperation=install\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_PACKAGE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_require_none());

    write_journal("format=1\noperation=remove\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_PACKAGE, kind);

    write_journal("format=1\noperation=update\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_PACKAGE, kind);

    write_journal("format=1\noperation=cup-update\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_CUP_UPDATE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_require_none());

    write_journal("format=1\noperation=uninstall\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_UNINSTALL, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_require_none());

    clear_current_journal();
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_MISSING, kind);
    clear_current_journal();
}

static void test_rejects_invalid(void) {
    RuntimeJournalKind kind;
    char long_record[700];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, runtime_journal_detect(NULL));
    write_journal("not-key-value\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\noperation =install\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("operation=install\noperation=remove\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\noperation=unknown-operation\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    memset(long_record, 'x', sizeof(long_record));
    long_record[sizeof(long_record) - 2] = '\n';
    long_record[sizeof(long_record) - 1] = '\0';
    write_journal(long_record);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    {
        static const unsigned char hidden_nul[] =
            "format=1\noperation=install\ntemporary_name=x\0\n";
        write_journal_bytes(hidden_nul, sizeof(hidden_nul) - 1);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
    }

    write_journal("format=1\noperation=install\ntemporary_name=x");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\r\noperation=install\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
}

static void test_storage_failures(void) {
    RuntimeJournalKind kind;
    SystemPathIdentity identity;
    int missing;

    path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));

    path_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(journal_path, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(0, test_rmdir(journal_path));

    write_journal("format=1\noperation=install\n");
    remove_identity_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_clear_if_identity(&identity));

    remove_identity_result = CUP_OK;
    sync_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, runtime_journal_clear_if_identity(&identity));
    TEST_ASSERT_FALSE(test_access_exists(journal_path));
}

static void test_failure_outputs_are_cleared(void) {
    SystemPathIdentity identity;
    SystemPathIdentity expected;
    int missing;

    write_journal("format=1\noperation=install\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));
    TEST_ASSERT_TRUE(identity.valid);

    path_result = CUP_ERR_BUFFER_TOO_SMALL;
    memset(&identity, 0xff, sizeof(identity));
    missing = 7;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_parse(NULL, 0, accept_journal_field, NULL, &identity, &missing));
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_INT(0, missing);

    memset(&identity, 0xff, sizeof(identity));
    missing = 7;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          runtime_journal_parse(NULL, 0, NULL, NULL, &identity, &missing));
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_INT(0, missing);

    path_result = CUP_OK;
    memset(&identity, 0xff, sizeof(identity));
    missing = 7;
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          runtime_journal_parse(
                              NULL, 0, accept_journal_field, NULL, &identity, &missing));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_EQUAL_INT(0, missing);

    memset(&expected, 0, sizeof(expected));
    expected.valid = 1;
    expected.kind = SYSTEM_PATH_DIRECTORY;
    memset(&identity, 0xff, sizeof(identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_publish(journal_root,
                                "transaction",
                                &expected,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          runtime_journal_clear_if_identity(&expected));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
}

static void test_publishes_and_replaces_by_identity(void) {
    SystemPathIdentity identity;
    SystemPathIdentity replacement;
    RuntimeJournalKind kind;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        runtime_journal_publish(journal_root,
                                "transaction",
                                NULL,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_PACKAGE, kind);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        runtime_journal_publish(journal_root,
                                "transaction",
                                &identity,
                                write_test_journal,
                                "cup-update",
                                &replacement));
    TEST_ASSERT_TRUE(replacement.valid);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_CUP_UPDATE, kind);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_publish(NULL,
                                "transaction",
                                NULL,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_publish(journal_root,
                                "transaction",
                                NULL,
                                NULL,
                                "install",
                                &identity));
}

static void test_publish_commit_boundaries(void) {
    SystemPathIdentity identity;

    publish_result = CUP_ERR_FILESYSTEM;
    publish_state = SYSTEM_COMMIT_NOT_APPLIED;
    memset(&identity, 0xff, sizeof(identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_publish(journal_root,
                                "transaction",
                                NULL,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_TRUE(!test_access_exists(journal_path));

    publish_state = SYSTEM_COMMIT_APPLIED;
    memset(&identity, 0xff, sizeof(identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_COMMIT,
        runtime_journal_publish(journal_root,
                                "transaction",
                                NULL,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_TRUE(identity.valid);
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
}

static void test_identity_mismatch_preserves_journal(void) {
    SystemPathIdentity expected = {0};

    write_journal("format=1\noperation=install\n");
    expected.kind = SYSTEM_PATH_REGULAR_FILE;
    expected.valid = 1;
    remove_identity_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION, runtime_journal_clear_if_identity(&expected));
    TEST_ASSERT_TRUE(test_access_exists(journal_path));
}

static void test_first_publish_preserves_existing_journal(void) {
    SystemPathIdentity identity;
    RuntimeJournalKind kind;

    write_journal("format=1\noperation=cup-update\ntemporary_name=x\n");
    memset(&identity, 0xff, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        runtime_journal_publish(journal_root,
                                "transaction",
                                NULL,
                                write_test_journal,
                                "install",
                                &identity));
    TEST_ASSERT_FALSE(identity.valid);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_CUP_UPDATE, kind);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_detects_owners);
    RUN_TEST(test_rejects_invalid);
    RUN_TEST(test_storage_failures);
    RUN_TEST(test_failure_outputs_are_cleared);
    RUN_TEST(test_publishes_and_replaces_by_identity);
    RUN_TEST(test_publish_commit_boundaries);
    RUN_TEST(test_identity_mismatch_preserves_journal);
    RUN_TEST(test_first_publish_preserves_existing_journal);
    return UNITY_END();
}
