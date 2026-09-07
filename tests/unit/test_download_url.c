/* Exercises release-base override parsing and the test-only loopback policy. */

#include "unity.h"

#include "download.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static void set_test_environment(const char *name, const char *value) {
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(name, value == NULL ? "" : value));
}
#else
static void set_test_environment(const char *name, const char *value) {
    int result = value == NULL ? unsetenv(name) : setenv(name, value, 1);
    TEST_ASSERT_EQUAL_INT(0, result);
}
#endif

void setUp(void) {
    set_test_environment("CUP_INSTALL_BASE_URL", NULL);
    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", NULL);
}

void tearDown(void) {
    set_test_environment("CUP_INSTALL_BASE_URL", NULL);
    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", NULL);
}

static void expect_override_invalid(const char *url) {
    char base[256] = "stale";
    set_test_environment("CUP_INSTALL_BASE_URL", url);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_copy_release_base_override(base, sizeof(base)));
    TEST_ASSERT_EQUAL_STRING("", base);
}

static void test_release_base_override_policy(void) {
    char base[256];
    char small[8];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          download_copy_release_base_override(base, sizeof(base)));
    TEST_ASSERT_EQUAL_STRING("", base);

    set_test_environment("CUP_INSTALL_BASE_URL", "http://127.0.0.1:18080/path/");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_copy_release_base_override(base, sizeof(base)));
    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", "1");
    TEST_ASSERT_EQUAL_INT(CUP_OK, download_copy_release_base_override(base, sizeof(base)));
    TEST_ASSERT_EQUAL_STRING("http://127.0.0.1:18080/path", base);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          download_copy_release_base_override(small, sizeof(small)));

    expect_override_invalid("https://127.0.0.1:18080");
    expect_override_invalid("http://example.invalid:18080");
    expect_override_invalid("http://127.0.0.1");
    expect_override_invalid("http://127.0.0.1:0");
    expect_override_invalid("http://user@127.0.0.1:18080");
    expect_override_invalid("http://127.0.0.1:18080/path?query");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_copy_release_base_override(NULL, sizeof(base)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          download_copy_release_base_override(base, 0));
}

static void test_loopback_transport_policy(void) {
    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", "1");
    TEST_ASSERT_TRUE(download_insecure_loopback_is_allowed("http://127.0.0.1:18080/resource"));
    TEST_ASSERT_FALSE(download_insecure_loopback_is_allowed("http://localhost:18080/resource"));
    TEST_ASSERT_FALSE(download_insecure_loopback_is_allowed("https://127.0.0.1:18080/resource"));

    set_test_environment("CUP_INSTALL_ALLOW_INSECURE", NULL);
    TEST_ASSERT_FALSE(download_insecure_loopback_is_allowed("http://127.0.0.1:18080/resource"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_release_base_override_policy);
    RUN_TEST(test_loopback_transport_policy);
    return UNITY_END();
}
