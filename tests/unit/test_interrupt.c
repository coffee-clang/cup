/*
 * Exercises the real POSIX signal lifecycle owned by interrupt.c.
 */

#include "interrupt.h"
#include "unity.h"

#include <signal.h>

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
    TEST_ASSERT_EQUAL_INT(0, raise(SIGINT));
    TEST_ASSERT_TRUE(interrupt_requested());
    interrupt_clear();
    TEST_ASSERT_FALSE(interrupt_requested());
    interrupt_disable();
    TEST_ASSERT_FALSE(interrupt_requested());
}

static void test_sigterm(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_EQUAL_INT(0, raise(SIGTERM));
    TEST_ASSERT_TRUE(interrupt_requested());
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lifecycle);
    RUN_TEST(test_sigterm);
    return UNITY_END();
}
