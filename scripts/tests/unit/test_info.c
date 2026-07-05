#include "error.h"
#include "info.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char temp_dir[] = "/tmp/cup-info-test-XXXXXX";

void setUp(void) {}
void tearDown(void) {}

static void build_path(char *out, size_t size, const char *name) {
    TEST_ASSERT_TRUE(snprintf(out, size, "%s/%s", temp_dir, name) > 0);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_info_load_reads_fields_and_prefix_groups(void) {
    PackageInfo info;
    const PackageInfoField *field;
    char path[256];
    size_t cursor = 0;

    info_init(&info);
    build_path(path, sizeof(path), "info.txt");
    write_text(path,
        "package.component=compiler\n"
        "package.tool=clang\n"
        "entry.clang=bin/clang\n"
        "entry.clang++=bin/clang++\n"
        "features.lto=yes\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, info_load(&info, path));
    TEST_ASSERT_EQUAL_STRING("compiler", info_get(&info, "package.component"));
    TEST_ASSERT_EQUAL_STRING("clang", info_get(&info, "package.tool"));
    TEST_ASSERT_NULL(info_get(&info, "missing"));

    field = info_next(&info, "entry.", &cursor);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_EQUAL_STRING("entry.clang", field->key);
    TEST_ASSERT_EQUAL_STRING("bin/clang", field->value);

    field = info_next(&info, "entry.", &cursor);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_EQUAL_STRING("entry.clang++", field->key);
    TEST_ASSERT_EQUAL_STRING("bin/clang++", field->value);

    TEST_ASSERT_NULL(info_next(&info, "entry.", &cursor));
    info_free(&info);
}

static void test_info_load_rejects_duplicate_and_unsafe_keys(void) {
    PackageInfo info;
    char path[256];

    info_init(&info);
    build_path(path, sizeof(path), "duplicate.txt");
    write_text(path,
        "package.component=compiler\n"
        "package.component=debugger\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, info_load(&info, path));
    TEST_ASSERT_EQUAL_size_t(0, info.count);

    build_path(path, sizeof(path), "unsafe-key.txt");
    write_text(path, "entry...clang=bin/clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, info_load(&info, path));

    build_path(path, sizeof(path), "unsafe-value.txt");
    write_text(path, "package.component=\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, info_load(&info, path));

    info_free(&info);
}

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_info_load_reads_fields_and_prefix_groups);
    RUN_TEST(test_info_load_rejects_duplicate_and_unsafe_keys);
    return UNITY_END();
}
