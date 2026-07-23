/*
 * Test focus: Exercises strict info.txt parsing, storage growth, duplicate rejection and ordered
 * field lookup.
 */

#include "error.h"
#include "package_metadata.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
}

void tearDown(void) {
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_text(const char *path, const char *text) {
    write_bytes(path, text, strlen(text));
}

/* Test cases grouped by the public contract they exercise. */

static void test_load_fields(void) {
    PackageMetadata info;
    const PackageMetadataField *field;
    char path[256];
    size_t cursor = 0;

    package_metadata_init(&info);
    build_path(path, sizeof(path), "info.txt");
    write_text(path,
               "# package metadata\n"
               "package.component=compiler\r\n"
               "package.tool=clang\n"
               "entry.clang=bin/clang\n"
               "entry.clang++=bin/clang++\n"
               "features.lto=yes\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_metadata_load(&info, path));
    TEST_ASSERT_EQUAL_size_t(5, info.count);
    TEST_ASSERT_EQUAL_STRING("compiler", package_metadata_get(&info, "package.component"));
    TEST_ASSERT_EQUAL_STRING("clang", package_metadata_get(&info, "package.tool"));
    TEST_ASSERT_NULL(package_metadata_get(&info, "missing"));

    field = package_metadata_next(&info, "entry.", &cursor);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_EQUAL_STRING("entry.clang", field->key);
    field = package_metadata_next(&info, "entry.", &cursor);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_EQUAL_STRING("entry.clang++", field->key);
    TEST_ASSERT_NULL(package_metadata_next(&info, "entry.", &cursor));
    package_metadata_free(&info);
}

static void test_storage_growth(void) {
    PackageMetadata info;
    char path[256];
    FILE *file;
    size_t i;

    package_metadata_init(&info);
    build_path(path, sizeof(path), "many.txt");
    file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    for (i = 0; i < 20; ++i) {
        TEST_ASSERT_TRUE(fprintf(file, "config.key%zu=value%zu\n", i, i) > 0);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_metadata_load(&info, path));
    TEST_ASSERT_EQUAL_size_t(20, info.count);
    TEST_ASSERT_TRUE(info.capacity >= 20);
    TEST_ASSERT_EQUAL_STRING("value19", package_metadata_get(&info, "config.key19"));
    package_metadata_free(&info);
    TEST_ASSERT_NULL(info.fields);
    TEST_ASSERT_EQUAL_size_t(0, info.count);
    package_metadata_free(NULL);
    package_metadata_init(NULL);
}

static void test_invalid_fields(void) {
    PackageMetadata info;
    char path[256];
    char long_key[MAX_METADATA_KEY_LEN + 2];
    char long_value[MAX_METADATA_VALUE_LEN + 2];
    char record[MAX_METADATA_LINE_LEN];

    package_metadata_init(&info);
    build_path(path, sizeof(path), "duplicate.txt");
    write_text(path,
               "package.component=compiler\n"
               "package.component=debugger\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));
    TEST_ASSERT_EQUAL_size_t(0, info.count);

    build_path(path, sizeof(path), "unsafe-key.txt");
    write_text(path, "entry...clang=bin/clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    build_path(path, sizeof(path), "empty-group.txt");
    write_text(path, "features.=yes\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    build_path(path, sizeof(path), "empty-value.txt");
    write_text(path, "package.component=\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_TRUE(snprintf(record, sizeof(record), "%s=value\n", long_key) > 0);
    build_path(path, sizeof(path), "long-key.txt");
    write_text(path, record);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    memset(long_value, 'v', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    TEST_ASSERT_TRUE(snprintf(record, sizeof(record), "key=%s\n", long_value) > 0);
    build_path(path, sizeof(path), "long-value.txt");
    write_text(path, record);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    package_metadata_free(&info);
}

static void test_line_failures(void) {
    PackageMetadata info;
    char path[256];
    unsigned char control[] = {'k', 'e', 'y', '=', 'a', 1, 'b', '\n'};
    char long_line[MAX_METADATA_LINE_LEN + 16];

    package_metadata_init(&info);
    build_path(path, sizeof(path), "control.txt");
    write_bytes(path, control, sizeof(control));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\n';
    build_path(path, sizeof(path), "long-line.txt");
    write_bytes(path, long_line, sizeof(long_line));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));

    build_path(path, sizeof(path), "directory");
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_metadata_load(&info, path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(path));

    build_path(path, sizeof(path), "missing.txt");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_metadata_load(&info, path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_metadata_load(NULL, path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_metadata_load(&info, NULL));
    package_metadata_free(&info);
}

static void test_query_guards(void) {
    PackageMetadata info;
    char path[256];
    size_t cursor = 0;

    package_metadata_init(&info);
    build_path(path, sizeof(path), "query.txt");
    write_text(path, "entry.clang=bin/clang\npackage.tool=clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_metadata_load(&info, path));

    TEST_ASSERT_NULL(package_metadata_get(NULL, "package.tool"));
    TEST_ASSERT_NULL(package_metadata_get(&info, NULL));
    TEST_ASSERT_NULL(package_metadata_get(&info, ""));
    TEST_ASSERT_NULL(package_metadata_next(NULL, "entry.", &cursor));
    TEST_ASSERT_NULL(package_metadata_next(&info, NULL, &cursor));
    TEST_ASSERT_NULL(package_metadata_next(&info, "entry.", NULL));
    cursor = info.count;
    TEST_ASSERT_NULL(package_metadata_next(&info, "entry.", &cursor));
    package_metadata_free(&info);
}

/* Suite registration. */

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-info-test"));
    UNITY_BEGIN();
    RUN_TEST(test_load_fields);
    RUN_TEST(test_storage_growth);
    RUN_TEST(test_invalid_fields);
    RUN_TEST(test_line_failures);
    RUN_TEST(test_query_guards);
    return UNITY_END();
}
