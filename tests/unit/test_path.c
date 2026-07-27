/* Test focus: Exercises normalized paths and portable package-path validation. */

#include "error.h"
#include "path.h"
#include "text.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_path_segments(void) {
    static const char *const reserved[] = {
        "CON", "prn.txt", "Aux", "nul", "COM1", "com9.log", "LPT1", "lpt9.tmp"};
    size_t i;

    TEST_ASSERT_TRUE(path_is_safe_segment("clang-22.1.5"));
    TEST_ASSERT_TRUE(path_is_safe_identifier("clang-format_22.1.5+rev1"));
    TEST_ASSERT_FALSE(path_is_safe_segment(NULL));
    TEST_ASSERT_FALSE(path_is_safe_segment(""));
    TEST_ASSERT_FALSE(path_is_safe_segment("."));
    TEST_ASSERT_FALSE(path_is_safe_segment(".."));
    TEST_ASSERT_FALSE(path_is_safe_segment("name."));
    TEST_ASSERT_FALSE(path_is_safe_segment("name "));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad/name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad\\name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad:name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad?name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad*name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad\"name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad<name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad>name"));
    TEST_ASSERT_FALSE(path_is_safe_segment("bad|name"));
    {
        char control[] = {'a', 1, 'b', '\0'};
        char deleted[] = {'a', 127, 'b', '\0'};
        char utf8[] = {'a', (char)0xc3, (char)0xa9, '\0'};
        TEST_ASSERT_FALSE(path_is_safe_segment(control));
        TEST_ASSERT_FALSE(path_is_safe_segment(deleted));
        TEST_ASSERT_FALSE(path_is_safe_segment(utf8));
        TEST_ASSERT_FALSE(path_is_safe_segment("bad name"));
    }
    TEST_ASSERT_FALSE(path_is_safe_identifier("-starts"));
    TEST_ASSERT_FALSE(path_is_safe_identifier("bad name"));
    TEST_ASSERT_FALSE(path_is_safe_identifier("bad/name"));

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        TEST_ASSERT_FALSE(path_is_safe_segment(reserved[i]));
    }
}

static void test_relative_paths(void) {
    char long_segment[257];

    memset(long_segment, 'a', sizeof(long_segment) - 1);
    long_segment[sizeof(long_segment) - 1] = '\0';

    TEST_ASSERT_TRUE(path_is_safe_relative("root/bin/clang"));
    TEST_ASSERT_TRUE(path_is_safe_relative("single"));
    TEST_ASSERT_FALSE(path_is_safe_relative(NULL));
    TEST_ASSERT_FALSE(path_is_safe_relative(""));
    TEST_ASSERT_FALSE(path_is_safe_relative("/absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("\\absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("C:/absolute"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root:part"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root\\bin"));
    TEST_ASSERT_FALSE(path_is_safe_relative("../escape"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root//bin"));
    TEST_ASSERT_FALSE(path_is_safe_relative("root/"));
    TEST_ASSERT_FALSE(path_is_safe_relative(long_segment));
}

static void test_path_normalization(void) {
    char path[128];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_normalize(NULL));
    path[0] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_normalize(path));
    TEST_ASSERT_FALSE(path_equal(NULL, "/tmp"));
    TEST_ASSERT_FALSE(path_equal("/tmp", NULL));

#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, text_copy(path, sizeof(path), "C:\\Users\\Test\\.cup\\components\\"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(path));
    TEST_ASSERT_EQUAL_STRING("C:/Users/Test/.cup/components", path);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, text_copy(path, sizeof(path), "\\\\?\\C:\\Users\\Test\\.cup"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(path));
    TEST_ASSERT_EQUAL_STRING("C:/Users/Test/.cup", path);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, text_copy(path, sizeof(path), "\\\\?\\UNC\\server\\share\\cup"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(path));
    TEST_ASSERT_EQUAL_STRING("//server/share/cup", path);

    TEST_ASSERT_TRUE(path_equal("C:/Users/Test/.cup", "c:\\users\\test\\.cup\\"));
    TEST_ASSERT_TRUE(path_equal("//server/share/cup", "\\\\?\\UNC\\SERVER\\share\\cup"));
    TEST_ASSERT_FALSE(path_equal("C:/Users/Test/.cup", "C:/Users/Test/other"));

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), "C:\\Users\\Test", ".cup"));
    TEST_ASSERT_EQUAL_STRING("C:/Users/Test/.cup", path);
#else
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_copy(path, sizeof(path), "/tmp/cup\\name"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_normalize(path));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup\\name", path);
    TEST_ASSERT_TRUE(path_equal("/tmp/cup", "/tmp/cup"));
    TEST_ASSERT_FALSE(path_equal("/tmp/cup", "/TMP/CUP"));
    TEST_ASSERT_FALSE(path_equal("/tmp/cup/name", "/tmp/cup\\name"));
#endif
}

static void test_path_building(void) {
    char path[64];

    TEST_ASSERT_EQUAL_STRING("name", path_last_segment("name"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root/bin/clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root\\bin\\clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root\\bin/clang"));
    TEST_ASSERT_EQUAL_STRING("clang", path_last_segment("root/bin\\clang"));
    TEST_ASSERT_EQUAL_STRING("", path_last_segment("root/bin/"));
    TEST_ASSERT_NULL(path_last_segment(NULL));

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(path, sizeof(path), "/tmp/cup", "cache"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/cache", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join_safe_relative(path, sizeof(path), "/tmp/cup", "packages/gcc"));
    TEST_ASSERT_EQUAL_STRING("/tmp/cup/packages/gcc", path);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, sizeof(path), "/tmp/cup", "../escape"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(NULL, sizeof(path), "/tmp/cup", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, 0, "/tmp/cup", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          path_join_safe_relative(path, sizeof(path), "", "ok"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(NULL, sizeof(path), "/tmp", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, 0, "/tmp", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, sizeof(path), "", "cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_join(path, sizeof(path), "/tmp", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, path_join(path, 8, "/tmp/cup", "cache"));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_parent(path, sizeof(path), NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_parent(NULL, sizeof(path), "/tmp/cup"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, path_parent(path, 0, "/tmp/cup"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(path, sizeof(path), "name"));
    TEST_ASSERT_EQUAL_STRING(".", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(path, sizeof(path), "/name"));
    TEST_ASSERT_EQUAL_STRING("/", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(path, sizeof(path), "/tmp/cup"));
    TEST_ASSERT_EQUAL_STRING("/tmp", path);
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(path, sizeof(path), "C:\\cup\\bin"));
    TEST_ASSERT_EQUAL_STRING("C:/cup", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_parent(path, sizeof(path), "C:\\cup"));
    TEST_ASSERT_EQUAL_STRING("C:/", path);
#endif
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_path_segments);
    RUN_TEST(test_relative_paths);
    RUN_TEST(test_path_normalization);
    RUN_TEST(test_path_building);
    return UNITY_END();
}
