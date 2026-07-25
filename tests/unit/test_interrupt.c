/*
 * Test focus: Exercises the native interrupt-handler lifecycle. POSIX also
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
    interrupt_clear();
}

void tearDown(void) {
    interrupt_disable();
}

/* Test cases grouped by the public contract they exercise. */

static void test_lifecycle(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, interrupt_enable());
    TEST_ASSERT_FALSE(interrupt_requested());
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, raise(SIGINT));
    TEST_ASSERT_TRUE(interrupt_requested());
#endif
    interrupt_clear();
    TEST_ASSERT_FALSE(interrupt_requested());
    interrupt_disable();
    TEST_ASSERT_FALSE(interrupt_requested());
}

#if !defined(_WIN32)
static void test_sigterm(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_EQUAL_INT(0, raise(SIGTERM));
    TEST_ASSERT_TRUE(interrupt_requested());
}
#endif

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle);
#if !defined(_WIN32)
    RUN_TEST(test_sigterm);
#endif
    return UNITY_END();
}
