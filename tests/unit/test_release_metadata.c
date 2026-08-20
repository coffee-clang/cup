/*
 * Exercises canonical release versions and strict release.txt snapshots independently
 * from bootstrap and update orchestration.
 */

#include "release_metadata.h"
#include "test_platform.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
#define TEST_RELEASE_PATH_SIZE (CUP_TEST_TEMP_PATH_SIZE + 64)

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

static void test_release_version_parse(void) {
    ReleaseVersion version;

    TEST_ASSERT_EQUAL_INT(CUP_OK, release_version_parse("0.2.2", &version));
    TEST_ASSERT_EQUAL_INT(0, version.major);
    TEST_ASSERT_EQUAL_INT(2, version.minor);
    TEST_ASSERT_EQUAL_INT(2, version.patch);
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_version_parse("999999.0.42", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_version_parse("999999.999999.999999", &version));
    TEST_ASSERT_EQUAL_UINT(999999, version.major);
    TEST_ASSERT_EQUAL_UINT(999999, version.minor);
    TEST_ASSERT_EQUAL_UINT(999999, version.patch);

    version = (ReleaseVersion){7, 8, 9};
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_version_parse(NULL, &version));
    TEST_ASSERT_EQUAL_UINT(0, version.major);
    TEST_ASSERT_EQUAL_UINT(0, version.minor);
    TEST_ASSERT_EQUAL_UINT(0, version.patch);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_version_parse("", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("01.2.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.02.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.03", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.3.4", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1000000.2.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.1000000.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.1000000", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.-2.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("+1.2.3", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.3 ", &version));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("v1.2.3", &version));
}

static void test_load_canonical_metadata(void) {
    ReleaseMetadata metadata;
    char path[TEST_RELEASE_PATH_SIZE];

    build_path(path, sizeof(path), "release.txt");
    write_text(path,
               "format=1\n"
               "version=0.2.2\n"
               "commit=0123456789abcdef0123456789abcdef01234567\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("0.2.2", metadata.version);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef01234567", metadata.commit);

    write_text(path,
               "format=1\n"
               "version=0.2.2\n"
               "commit=0000000000000000000000000000000000000000\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000000000", metadata.commit);

    write_text(path,
               "format=1\n"
               "version=999999.999999.999999\n"
               "commit=ffffffffffffffffffffffffffffffffffffffff\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("999999.999999.999999", metadata.version);
    TEST_ASSERT_EQUAL_STRING("ffffffffffffffffffffffffffffffffffffffff", metadata.commit);
}

static void assert_invalid_metadata(const char *name, const char *contents) {
    ReleaseMetadata metadata;
    char path[TEST_RELEASE_PATH_SIZE];

    memset(&metadata, 0x7f, sizeof(metadata));
    build_path(path, sizeof(path), name);
    write_text(path, contents);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("", metadata.version);
    TEST_ASSERT_EQUAL_STRING("", metadata.commit);
}

static void test_reject_noncanonical_metadata(void) {
    assert_invalid_metadata("reordered.txt",
                            "version=0.2.2\n"
                            "format=1\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("duplicate.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("unknown.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "source=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("wrong-format.txt",
                            "format=2\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("space.txt",
                            "format=1\n"
                            "version=0.2.2 \n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("comment.txt",
                            "format=1\n"
                            "#x\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("blank-line.txt",
                            "format=1\n"
                            "\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("uppercase.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef0123456A\n");
    assert_invalid_metadata("short-commit.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=012345\n");
    assert_invalid_metadata("short-full-sha.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef0123456\n");
    assert_invalid_metadata("long-commit.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef012345678\n");
    assert_invalid_metadata("nonhex-commit.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef0123456g\n");
    assert_invalid_metadata("invalid-version.txt",
                            "format=1\n"
                            "version=0.02.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid_metadata("missing-newline.txt",
                            "format=1\n"
                            "version=0.2.2\n"
                            "commit=0123456789abcdef0123456789abcdef01234567");
}

static void test_load_failures(void) {
    ReleaseMetadata metadata;
    char path[TEST_RELEASE_PATH_SIZE];
    unsigned char oversized[1024];
    const unsigned char embedded_nul[] = {
        'f', 'o', 'r', 'm', 'a', 't', '=', '1', '\n',
        'v', 'e', 'r', 's', 'i', 'o', 'n', '=', '0', '.', '2', '.', '2', '\n',
        'c', 'o', 'm', 'm', 'i', 't', '=', '0', '1', '2', '3', '\0', '4', '\n'};

    build_path(path, sizeof(path), "missing.txt");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_metadata_load(NULL, &metadata));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_metadata_load(path, NULL));

    build_path(path, sizeof(path), "directory");
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_INT(0, test_rmdir(path));

    memset(oversized, 'x', sizeof(oversized));
    build_path(path, sizeof(path), "oversized.txt");
    write_bytes(path, oversized, sizeof(oversized));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("", metadata.version);
    TEST_ASSERT_EQUAL_STRING("", metadata.commit);

    build_path(path, sizeof(path), "embedded-nul.txt");
    write_bytes(path, embedded_nul, sizeof(embedded_nul));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_metadata_load(path, &metadata));
}

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-release-metadata-test"));
    UNITY_BEGIN();
    RUN_TEST(test_release_version_parse);
    RUN_TEST(test_load_canonical_metadata);
    RUN_TEST(test_reject_noncanonical_metadata);
    RUN_TEST(test_load_failures);
    return UNITY_END();
}
