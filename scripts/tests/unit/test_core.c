#include "entry.h"
#include "error.h"
#include "path.h"
#include "text.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_text_copy_and_format_report_size_errors(void) {
    char buffer[8];

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(buffer, sizeof(buffer), "cup"));
    TEST_ASSERT_EQUAL_STRING("cup", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
        text_copy(buffer, sizeof(buffer), "too-long-for-buffer"));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        text_format(buffer, sizeof(buffer), "%s", "0.2.0"));
    TEST_ASSERT_EQUAL_STRING("0.2.0", buffer);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
        text_format(buffer, sizeof(buffer), "%s", "0.2.0-dev"));
}

static void test_text_split_exact_trims_and_rejects_bad_counts(void) {
    char input[] = "  compiler . linux-x64 . windows-x64  ";
    char component[32];
    char host[32];
    char target[32];
    TextBuffer parts[3] = {
        {.data = component, .capacity = sizeof(component)},
        {.data = host, .capacity = sizeof(host)},
        {.data = target, .capacity = sizeof(target)}
    };

    TEST_ASSERT_EQUAL_INT(CUP_OK, text_split_exact(input, '.', parts, 3));
    TEST_ASSERT_EQUAL_STRING("compiler", component);
    TEST_ASSERT_EQUAL_STRING("linux-x64", host);
    TEST_ASSERT_EQUAL_STRING("windows-x64", target);

    {
        char too_few[] = "compiler.linux-x64";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
            text_split_exact(too_few, '.', parts, 3));
    }

    {
        char empty_part[] = "compiler..linux-x64";
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
            text_split_exact(empty_part, '.', parts, 3));
    }
}

static void test_entry_parse_and_build(void) {
    char tool[32];
    char release[32];
    char entry[64];

    TEST_ASSERT_TRUE(entry_is_stable("stable"));
    TEST_ASSERT_FALSE(entry_is_stable("22.1.5"));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        entry_parse("clang@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_STRING("clang", tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", release);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        entry_build(entry, sizeof(entry), "gcc", "16.1.0-rev1"));
    TEST_ASSERT_EQUAL_STRING("gcc@16.1.0-rev1", entry);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
        entry_parse("clang", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
        entry_parse("@22.1.5", tool, sizeof(tool), release, sizeof(release)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
        entry_parse("clang@", tool, sizeof(tool), release, sizeof(release)));
}

static void test_path_validation_rejects_unsafe_archive_paths(void) {
    TEST_ASSERT_TRUE(path_is_safe_segment("clang-22.1.5"));
    TEST_ASSERT_TRUE(path_is_safe_identifier("clang-format_22.1.5+rev1"));
    TEST_ASSERT_TRUE(path_is_safe_relative("root/bin/clang"));

    TEST_ASSERT_FALSE(path_is_safe_segment(".."));
    TEST_ASSERT_FALSE(path_is_safe_segment("NUL"));
    TEST_ASSERT_FALSE(path_is_safe_segment("name."));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad/name"));
    TEST_ASSERT_FALSE(path_is_safe_identifier("-starts-with-dash"));
    TEST_ASSERT_FALSE(path_is_safe_relative("/absolute/path"));
    TEST_ASSERT_FALSE(path_is_safe_relative("../escape"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root//bin"));
    TEST_ASSERT_FALSE(path_is_safe_relative("C:/absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root\\bin"));
}

static void test_path_join_keeps_safe_relative_contract(void) {
    char path[64];

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        path_join(path, sizeof(path), "/tmp/cup", "cache"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/cache", path);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        path_join_safe_relative(path, sizeof(path), "/tmp/cup", "packages/gcc"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/packages/gcc", path);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
        path_join_safe_relative(path, sizeof(path), "/tmp/cup", "../escape"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
        path_join(path, 8, "/tmp/cup", "cache"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_text_copy_and_format_report_size_errors);
    RUN_TEST(test_text_split_exact_trims_and_rejects_bad_counts);
    RUN_TEST(test_entry_parse_and_build);
    RUN_TEST(test_path_validation_rejects_unsafe_archive_paths);
    RUN_TEST(test_path_join_keeps_safe_relative_contract);
    return UNITY_END();
}
