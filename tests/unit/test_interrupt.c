/* Exercises one-shot native interrupt handling and safe-point observation. */

#include "interrupt.h"
#include "unity.h"

#if !defined(_WIN32)
#include <signal.h>
#endif

void setUp(void) {}
void tearDown(void) {}

static void test_interrupt_handler(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_enable());
    TEST_ASSERT_FALSE(interrupt_requested());
    TEST_ASSERT_EQUAL_INT(CUP_OK, interrupt_safe_point());
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, raise(SIGINT));
    TEST_ASSERT_TRUE(interrupt_requested());
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT, interrupt_safe_point());
#endif
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_interrupt_handler);
    return UNITY_END();
}
