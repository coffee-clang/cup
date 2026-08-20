/* Exercises symbolic and concrete package-selector parsing. */

#include "error.h"
#include "package_selector.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_selector_values(void) {
    char tool[32];
    char release[32];
    char entry[64];

    TEST_ASSERT_FALSE(package_release_is_stable(NULL));
    TEST_ASSERT_FALSE(package_release_is_stable(""));
    TEST_ASSERT_TRUE(package_release_is_stable("stable"));
    TEST_ASSERT_FALSE(package_release_is_stable("22.1.5"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_release_validate_concrete("22.1.5"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_release_validate_concrete("16.1.0-rev1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, package_release_validate_concrete("stable"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, package_release_validate_concrete("22.1.5-RC1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, package_release_validate_concrete("../22.1.5"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_release_validate_concrete(NULL));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_parse_parts("clang@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_STRING("clang", tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", release);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_selector_format_parts(entry, sizeof(entry), "gcc", "16.1.0-rev1"));
    TEST_ASSERT_EQUAL_STRING("gcc@16.1.0-rev1", entry);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang@", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("clang@1@2", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_parse_parts(" clang @1 ", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_STRING(" clang ", tool);
    TEST_ASSERT_EQUAL_STRING("1 ", release);
    {
        char long_entry[MAX_SELECTOR_LEN + 2];
        memset(long_entry, 'x', sizeof(long_entry) - 1);
        long_entry[sizeof(long_entry) - 1] = '\0';
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_BUFFER_TOO_SMALL,
            package_selector_parse_parts(long_entry, tool, sizeof(tool), release, sizeof(release)));
    }
    strcpy(tool, "keep");
    strcpy(release, "keep");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_selector_parse_parts(
                              "clang@22.1.5", tool, 3, release, sizeof(release)));
    TEST_ASSERT_EQUAL_STRING("keep", tool);
    TEST_ASSERT_EQUAL_STRING("keep", release);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_selector_format_parts(entry, 5, "gcc", "16"));
}

static void test_package_selectors(void) {
    PackageSelector selector;
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_selector_parse(&selector, "clang@stable"));
    TEST_ASSERT_EQUAL_STRING("clang", selector.tool);
    TEST_ASSERT_EQUAL_STRING("stable", selector.release);
    TEST_ASSERT_TRUE(package_release_is_stable(selector.release));

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_selector_parse(&selector, "gcc@16.1.0-rev1"));
    TEST_ASSERT_EQUAL_STRING("gcc", selector.tool);
    TEST_ASSERT_EQUAL_STRING("16.1.0-rev1", selector.release);
    TEST_ASSERT_FALSE(package_release_is_stable(selector.release));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL, package_selector_parse(&selector, "GCC@1"));
    TEST_ASSERT_EQUAL_STRING("", selector.tool);
    TEST_ASSERT_EQUAL_STRING("", selector.release);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_selector_parse(&selector, "gcc@STABLE"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          package_selector_parse(&selector, " gcc@stable"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_selector_parse(&selector, "gcc@1 "));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL, package_selector_parse(&selector, "../gcc@1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, package_selector_parse(&selector, "gcc@../1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_parse(NULL, "gcc@1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_selector_parse(&selector, "gcc"));
}

static void test_selector_guards(void) {
    char tool[8];
    char release[8];
    char entry[16];

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts(NULL, tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("gcc@1", NULL, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_parse_parts("gcc@1", tool, 0, release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_selector_parse_parts("gcc@1", tool, sizeof(tool), NULL, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_parse_parts("gcc@1", tool, sizeof(tool), release, 0));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(NULL, sizeof(entry), "gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, 0, "gcc", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, sizeof(entry), "", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_selector_format_parts(entry, sizeof(entry), "gcc", NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_selector_values);
    RUN_TEST(test_package_selectors);
    RUN_TEST(test_selector_guards);
    return UNITY_END();
}
