#define _POSIX_C_SOURCE 200809L
/* Exercises classification of the shared physical runtime journal. */

#include "runtime_journal.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char journal_path[] = "/tmp/cup-runtime-journal-test-XXXXXX";

void setUp(void) {
    int fd = mkstemp(journal_path);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    TEST_ASSERT_EQUAL_INT(0, unlink(journal_path));
}

void tearDown(void) {
    (void)unlink(journal_path);
    strcpy(journal_path, "/tmp/cup-runtime-journal-test-XXXXXX");
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "%s", journal_path) >= 0 && strlen(journal_path) < size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

static void write_journal(const char *contents) {
    FILE *file = fopen(journal_path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs(contents, file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_detects_owners(void) {
    RuntimeJournalKind kind;

    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_require_none());

    write_journal("format=1\noperation=install\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_PACKAGE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_require_none());

    write_journal("format=1\noperation=cup-update\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, runtime_journal_detect(&kind));
    TEST_ASSERT_EQUAL_INT(RUNTIME_JOURNAL_CUP_UPDATE, kind);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_require_none());
}

static void test_rejects_invalid(void) {
    RuntimeJournalKind kind;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, runtime_journal_detect(NULL));
    write_journal("not-key-value\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("operation=install\noperation=remove\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\noperation=self-update\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));

    write_journal("format=1\ntemporary_name=x\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, runtime_journal_detect(&kind));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_detects_owners);
    RUN_TEST(test_rejects_invalid);
    return UNITY_END();
}
