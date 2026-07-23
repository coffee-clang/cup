/*
 * Test focus: Exercises the closed package-archive format domain and bounded structural
 * preflight with real libarchive files.
 */

#include "package_archive.h"

#include "unity.h"
#include "test_platform.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[] = "/tmp/cup-package-archive-test-XXXXXX";

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
    if (size > 0) {
        TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void configure_writer(struct archive *writer, const char *format) {
    if (strcmp(format, "zip") == 0) {
        TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_set_format_zip(writer));
        return;
    }

    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_set_format_pax_restricted(writer));
    if (strcmp(format, "tar.gz") == 0) {
        TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_add_filter_gzip(writer));
    } else if (strcmp(format, "tar.xz") == 0) {
        TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_add_filter_xz(writer));
    }
}

static void create_archive(const char *path, const char *format, int include_payload) {
    struct archive *writer = archive_write_new();
    struct archive_entry *entry;
    static const char payload[] = "payload\n";

    TEST_ASSERT_NOT_NULL(writer);
    configure_writer(writer, format);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_open_filename(writer, path));

    entry = archive_entry_new();
    TEST_ASSERT_NOT_NULL(entry);
    archive_entry_set_pathname(entry, "package/");
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, 0);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_header(writer, entry));
    archive_entry_free(entry);

    if (include_payload) {
        entry = archive_entry_new();
        TEST_ASSERT_NOT_NULL(entry);
        archive_entry_set_pathname(entry, "package/info.txt");
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, (la_int64_t)(sizeof(payload) - 1));
        TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_header(writer, entry));
        TEST_ASSERT_EQUAL_INT((int)(sizeof(payload) - 1),
                              (int)archive_write_data(writer, payload, sizeof(payload) - 1));
        archive_entry_free(entry);
    }

    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_close(writer));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_free(writer));
}

/* Test cases grouped by the public contract they exercise. */

static void test_format_parser(void) {
    PackageArchiveFormat format = PACKAGE_ARCHIVE_FORMAT_ANY;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_parse_format("tar.xz", &format));
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_TAR_XZ, format);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_parse_format("tar.gz", &format));
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_TAR_GZ, format);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_parse_format("zip", &format));
    TEST_ASSERT_EQUAL_INT(PACKAGE_ARCHIVE_FORMAT_ZIP, format);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_archive_parse_format("tar", &format));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_archive_parse_format("7z", &format));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_parse_format(NULL, &format));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_parse_format("zip", NULL));
}

static void test_reader_contract(void) {
    struct archive *reader = (struct archive *)1;
    char valid[512];
    char invalid[512];

    build_path(valid, sizeof(valid), "reader.tar.gz");
    build_path(invalid, sizeof(invalid), "invalid.bin");
    create_archive(valid, "tar.gz", 1);
    write_bytes(invalid, "not an archive", 14);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_open_reader(NULL, valid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_open_reader(&reader, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE, package_archive_open_reader(&reader, invalid));
    TEST_ASSERT_NULL(reader);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_open_reader(&reader, valid));
    TEST_ASSERT_NOT_NULL(reader);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_read_close(reader));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_read_free(reader));
}

static void assert_validity(const char *path, const char *format, int expected) {
    int valid = !expected;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_is_valid(path, format, &valid));
    TEST_ASSERT_EQUAL_INT(expected, valid);
}

static void test_real_formats(void) {
    char tar_gz[512];
    char tar_xz[512];
    char zip[512];

    build_path(tar_gz, sizeof(tar_gz), "valid.tar.gz");
    build_path(tar_xz, sizeof(tar_xz), "valid.tar.xz");
    build_path(zip, sizeof(zip), "valid.zip");
    create_archive(tar_gz, "tar.gz", 1);
    create_archive(tar_xz, "tar.xz", 1);
    create_archive(zip, "zip", 1);

    assert_validity(tar_gz, "tar.gz", 1);
    assert_validity(tar_xz, "tar.xz", 1);
    assert_validity(zip, "zip", 1);
    assert_validity(tar_gz, NULL, 1);
    assert_validity(tar_xz, NULL, 1);
    assert_validity(zip, NULL, 1);
}

static void test_format_mismatch(void) {
    char tar_gz[512];
    char tar_xz[512];
    char zip[512];
    char plain_tar[512];

    build_path(tar_gz, sizeof(tar_gz), "mismatch.tar.gz");
    build_path(tar_xz, sizeof(tar_xz), "mismatch.tar.xz");
    build_path(zip, sizeof(zip), "mismatch.zip");
    build_path(plain_tar, sizeof(plain_tar), "plain.tar");
    create_archive(tar_gz, "tar.gz", 1);
    create_archive(tar_xz, "tar.xz", 1);
    create_archive(zip, "zip", 1);
    create_archive(plain_tar, "tar", 1);

    assert_validity(tar_gz, "tar.xz", 0);
    assert_validity(tar_xz, "zip", 0);
    assert_validity(zip, "tar.gz", 0);
    assert_validity(plain_tar, NULL, 0);

    {
        int valid = 1;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_archive_is_valid(zip, "7z", &valid));
        TEST_ASSERT_FALSE(valid);
    }
}

static void test_payload_required(void) {
    char missing[512];
    char empty[512];
    char invalid[512];
    char directory[512];
    char directories_only[512];
    int valid = 1;

    build_path(missing, sizeof(missing), "missing.tar.gz");
    build_path(empty, sizeof(empty), "empty.tar.gz");
    build_path(invalid, sizeof(invalid), "garbage.tar.gz");
    build_path(directory, sizeof(directory), "directory");
    build_path(directories_only, sizeof(directories_only), "directories.tar.gz");

    write_bytes(empty, "", 0);
    write_bytes(invalid, "garbage", 7);
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(directory, 0755));
    create_archive(directories_only, "tar.gz", 0);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_is_valid(NULL, "tar.gz", &valid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_archive_is_valid(missing, "tar.gz", NULL));
    assert_validity(missing, "tar.gz", 0);
    assert_validity(directory, "tar.gz", 0);
    assert_validity(empty, "tar.gz", 0);
    assert_validity(invalid, "tar.gz", 0);
    assert_validity(directories_only, "tar.gz", 0);
}

/* Suite registration. */

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_format_parser);
    RUN_TEST(test_reader_contract);
    RUN_TEST(test_real_formats);
    RUN_TEST(test_format_mismatch);
    RUN_TEST(test_payload_required);
    return UNITY_END();
}
