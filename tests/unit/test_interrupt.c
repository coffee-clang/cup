/*
 * Exercises the native interrupt-handler lifecycle. POSIX also
 * verifies real SIGINT/SIGTERM delivery; Windows console delivery remains an
 * integration boundary because it requires a dedicated console process group.
 */

#include "interrupt.h"
#include "unity.h"

#if !defined(_WIN32)
#include <signal.h>
#endif

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
    interrupt_disable();
}

void tearDown(void) {
    interrupt_disable();
}

/* Test cases grouped by the public contract they exercise. */

static void test_lifecycle(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, interrupt_enable());
    TEST_ASSERT_FALSE(interrupt_requested());
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_safe_point());
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, raise(SIGINT));
    TEST_ASSERT_TRUE(interrupt_requested());
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, interrupt_safe_point());
#endif
    interrupt_disable();
    TEST_ASSERT_FALSE(interrupt_requested());
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_FALSE(interrupt_requested());
    interrupt_disable();
}

#if !defined(_WIN32)
static void test_sigterm(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_EQUAL_INT(0, raise(SIGTERM));
    TEST_ASSERT_TRUE(interrupt_requested());
}

static void test_restores_previous_dispositions(void) {
    struct sigaction original_sigint;
    struct sigaction original_sigterm;
    struct sigaction ignored = {0};
    struct sigaction restored_sigint;
    struct sigaction restored_sigterm;
    int get_original_sigint;
    int get_original_sigterm;
    int set_sigint;
    int set_sigterm;
    CupError enable_result = CUP_ERR_FILESYSTEM;
    int get_restored_sigint = -1;
    int get_restored_sigterm = -1;
    int restore_sigint = -1;
    int restore_sigterm = -1;

    get_original_sigint = sigaction(SIGINT, NULL, &original_sigint);
    get_original_sigterm = sigaction(SIGTERM, NULL, &original_sigterm);
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    ignored.sa_flags = 0;

    set_sigint = get_original_sigint == 0 ? sigaction(SIGINT, &ignored, NULL) : -1;
    set_sigterm = get_original_sigterm == 0 ? sigaction(SIGTERM, &ignored, NULL) : -1;
    if (set_sigint == 0 && set_sigterm == 0) {
        enable_result = interrupt_enable();
        if (enable_result == CUP_OK) {
            interrupt_disable();
            get_restored_sigint = sigaction(SIGINT, NULL, &restored_sigint);
            get_restored_sigterm = sigaction(SIGTERM, NULL, &restored_sigterm);
        }
    }

    if (get_original_sigint == 0) {
        restore_sigint = sigaction(SIGINT, &original_sigint, NULL);
    }
    if (get_original_sigterm == 0) {
        restore_sigterm = sigaction(SIGTERM, &original_sigterm, NULL);
    }

    TEST_ASSERT_EQUAL_INT(0, get_original_sigint);
    TEST_ASSERT_EQUAL_INT(0, get_original_sigterm);
    TEST_ASSERT_EQUAL_INT(0, set_sigint);
    TEST_ASSERT_EQUAL_INT(0, set_sigterm);
    TEST_ASSERT_EQUAL_INT(CUP_OK, enable_result);
    TEST_ASSERT_EQUAL_INT(0, get_restored_sigint);
    TEST_ASSERT_EQUAL_INT(0, get_restored_sigterm);
    TEST_ASSERT_TRUE(restored_sigint.sa_handler == SIG_IGN);
    TEST_ASSERT_TRUE(restored_sigterm.sa_handler == SIG_IGN);
    TEST_ASSERT_EQUAL_INT(0, restore_sigint);
    TEST_ASSERT_EQUAL_INT(0, restore_sigterm);
}
#endif


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle);
#if !defined(_WIN32)
    RUN_TEST(test_sigterm);
    RUN_TEST(test_restores_previous_dispositions);
#endif
    return UNITY_END();
}
