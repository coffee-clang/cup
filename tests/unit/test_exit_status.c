/*
 * Tests the stable public process status mapping.
 */
#include "exit_status.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_success(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_SUCCESS, cup_error_to_exit_status(CUP_OK));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_SUCCESS, cup_error_to_exit_status(CUP_ERR_ALREADY_INSTALLED));
}

static void test_usage(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_INPUT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_TOOL));
}

static void test_availability_state(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_UNAVAILABLE, cup_error_to_exit_status(CUP_ERR_NOT_AVAILABLE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_INCONSISTENT_STATE));
}

static void test_network_operation(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_NETWORK, cup_error_to_exit_status(CUP_ERR_TLS));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_FILESYSTEM));
}

static void test_internal(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_INTERNAL, cup_error_to_exit_status(CUP_ERR_BUFFER_TOO_SMALL));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_INTERNAL, cup_error_to_exit_status((CupError)999));
}

static void test_interrupt(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_INTERRUPT, cup_error_to_exit_status(CUP_ERR_INTERRUPT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_success);
    RUN_TEST(test_usage);
    RUN_TEST(test_availability_state);
    RUN_TEST(test_network_operation);
    RUN_TEST(test_internal);
    RUN_TEST(test_interrupt);
    return UNITY_END();
}
