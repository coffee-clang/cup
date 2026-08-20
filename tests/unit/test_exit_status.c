/*
 * Tests the stable public process status mapping.
 */
#include "exit_status.h"
#include "unity.h"

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
}

void tearDown(void) {
}

/* Test cases grouped by the public contract they exercise. */

static void test_success(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_SUCCESS, cup_error_to_exit_status(CUP_OK));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_SUCCESS, cup_error_to_exit_status(CUP_ERR_ALREADY_INSTALLED));
}

static void test_usage(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_INPUT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE,
                          cup_error_to_exit_status(CUP_ERR_UNSUPPORTED_COMPONENT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_TOOL));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_RELEASE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_OS));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_USAGE, cup_error_to_exit_status(CUP_ERR_INVALID_ARCH));
}

static void test_availability_state(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_UNAVAILABLE, cup_error_to_exit_status(CUP_ERR_NOT_AVAILABLE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_UNAVAILABLE, cup_error_to_exit_status(CUP_ERR_NOT_INSTALLED));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_CATALOG));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_STATE_LOAD));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_STATE_FULL));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_DEFAULT_FULL));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_INCONSISTENT_STATE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_STATE, cup_error_to_exit_status(CUP_ERR_VALIDATION));
}

static void test_network_operation(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_NETWORK, cup_error_to_exit_status(CUP_ERR_FETCH));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_NETWORK, cup_error_to_exit_status(CUP_ERR_TLS));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_NETWORK, cup_error_to_exit_status(CUP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_NETWORK,
                          cup_error_to_exit_status(CUP_ERR_DOWNLOAD_TOO_LARGE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_FILESYSTEM));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_TEMPORARY));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_LOCK));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_TRANSACTION));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_ARCHIVE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_ARCHIVE_UNSAFE));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_EXTRACT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_COMMIT));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_OPERATION, cup_error_to_exit_status(CUP_ERR_ROLLBACK));
}

static void test_internal(void) {
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_INTERNAL, cup_error_to_exit_status(CUP_ERR_BUFFER_TOO_SMALL));
    TEST_ASSERT_EQUAL_INT(CUP_STATUS_INTERNAL,
                          cup_error_to_exit_status((CupError)(CUP_ERR_INTERRUPT + 1)));
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
