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
static CupError exists_result;
static CupError remove_result;
static CupError sync_result;

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
    exists_result = CUP_OK;
    remove_result = CUP_OK;
    sync_result = CUP_OK;
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

CupError system_path_exists(const char *path, int *exists) {
    if (path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (exists_result != CUP_OK) {
        return exists_result;
    }
    *exists = test_access_exists(path);
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    if (remove_result != CUP_OK) {
        return remove_result;
    }
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    return path == NULL ? CUP_ERR_INVALID_INPUT : sync_result;
}

static void write_journal(const char *contents) {
    FILE *file = fopen(journal_path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(contents, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
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

    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_clear());
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_clear());
}

static void test_rejects_invalid(void) {
    RuntimeJournalKind kind;
    char long_record[700];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, runtime_journal_detect(NULL));
    write_journal("not-key-value\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("operation=install\noperation=remove\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\noperation=unknown-operation\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    memset(long_record, 'x', sizeof(long_record));
    long_record[sizeof(long_record) - 1] = '\0';
    write_journal(long_record);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
}

static void test_storage_failures(void) {
    RuntimeJournalKind kind;

    path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_clear());

    path_result = CUP_OK;
    exists_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_clear());

    exists_result = CUP_OK;
    write_journal("format=1\noperation=install\n");
    remove_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_clear());

    remove_result = CUP_OK;
    sync_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_clear());
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_detects_owners);
    RUN_TEST(test_rejects_invalid);
    RUN_TEST(test_storage_failures);
    return UNITY_END();
}
