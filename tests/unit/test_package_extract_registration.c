/*
 * Exercises path-registration resource failure through the real extraction entry point. The test
 * replaces only uthash's private allocator so production needs no fault-injection hook.
 */

#include "package_archive.h"
#include "package_extract.h"
#include "test_platform.h"
#include "unity.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_hash_allocation;
static char root[CUP_TEST_TEMP_PATH_SIZE];

static void *test_uthash_malloc(size_t size) {
    return fail_hash_allocation ? NULL : malloc(size);
}

#define uthash_malloc(size) test_uthash_malloc(size)
#include "../../src/package_extract.c"
#undef uthash_malloc

int interrupt_requested(void) {
    return 0;
}

int package_archive_reader_matches_format(struct archive *reader, PackageArchiveFormat expected) {
    (void)reader;
    return expected == PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
}

CupError package_archive_open_stream(struct archive **reader, FILE *file) {
    struct archive *candidate = archive_read_new();

    if (candidate == NULL || file == NULL) {
        archive_read_free(candidate);
        return CUP_ERR_ARCHIVE;
    }
    rewind(file);
    archive_read_support_filter_all(candidate);
    archive_read_support_format_all(candidate);
    if (archive_read_open_FILE(candidate, file) != ARCHIVE_OK) {
        archive_read_free(candidate);
        return CUP_ERR_ARCHIVE;
    }
    *reader = candidate;
    return CUP_OK;
}

static void join_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void create_archive(char *path, size_t path_size) {
    struct archive *writer = archive_write_new();
    struct archive_entry *entry = archive_entry_new();
    static const char content[] = "x";

    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_set_format_pax_restricted(writer));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_add_filter_gzip(writer));
    TEST_ASSERT_TRUE(snprintf(path, path_size, "%s/registration.tar.gz", root) > 0);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_open_filename(writer, path));

    archive_entry_set_pathname(entry, "pkg/tool");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, 1);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_header(writer, entry));
    TEST_ASSERT_EQUAL_INT(1, (int)archive_write_data(writer, content, 1));
    archive_entry_free(entry);

    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_close(writer));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_free(writer));
}

static char *capture_resource_failure(const char *archive_path,
                                      const char *destination,
                                      CupError *result) {
    VerifiedArtifact artifact;
    FILE *capture = tmpfile();
    int saved;
    long length;
    char *output;

    TEST_ASSERT_NOT_NULL(capture);
    memset(&artifact, 0, sizeof(artifact));
    artifact.file = fopen(archive_path, "rb");
    TEST_ASSERT_NOT_NULL(artifact.file);
    artifact.format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;

    TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
    saved = test_dup_fd(TEST_PLATFORM_STDERR_FD);
    TEST_ASSERT_TRUE(saved >= 0);
    TEST_ASSERT_TRUE(test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDERR_FD) >= 0);

    fail_hash_allocation = 1;
    *result = package_extract_verified(&artifact, destination);
    fail_hash_allocation = 0;

    TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
    TEST_ASSERT_TRUE(test_dup2_fd(saved, TEST_PLATFORM_STDERR_FD) >= 0);
    TEST_ASSERT_EQUAL_INT(0, test_close_fd(saved));
    TEST_ASSERT_EQUAL_INT(0, fclose(artifact.file));

    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_END));
    length = ftell(capture);
    TEST_ASSERT_TRUE(length >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    output = calloc((size_t)length + 1, 1);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_size_t((size_t)length, fread(output, 1, (size_t)length, capture));
    TEST_ASSERT_FALSE(ferror(capture));
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    return output;
}

void setUp(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(root, sizeof(root), "cup-extract-register"));
    fail_hash_allocation = 0;
}

void tearDown(void) {
    fail_hash_allocation = 0;
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(root));
}

static void test_declared_size_limits_are_owned_by_extraction(void) {
    struct archive_entry *entry = archive_entry_new();
    uint64_t total;

    TEST_ASSERT_NOT_NULL(entry);
    archive_entry_set_pathname(entry, "pkg/tool");
    archive_entry_set_filetype(entry, AE_IFREG);

    total = 0;
    archive_entry_set_size(entry, (la_int64_t)MAX_PACKAGE_ENTRY_BYTES + 1);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE, validate_entry_size(entry, &total));
    TEST_ASSERT_EQUAL_UINT64(0, total);

    total = MAX_PACKAGE_EXTRACTED_BYTES - 1;
    archive_entry_set_size(entry, 2);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ARCHIVE_UNSAFE, validate_entry_size(entry, &total));
    TEST_ASSERT_EQUAL_UINT64(MAX_PACKAGE_EXTRACTED_BYTES - 1, total);

    total = 7;
    archive_entry_set_size(entry, 3);
    TEST_ASSERT_EQUAL_INT(CUP_OK, validate_entry_size(entry, &total));
    TEST_ASSERT_EQUAL_UINT64(10, total);

    archive_entry_free(entry);
}

static void test_actual_decoded_limit_is_rechecked_before_write(void) {
    char archive_path[CUP_TEST_TEMP_PATH_SIZE];
    FILE *file;
    struct archive *reader = NULL;
    struct archive_entry *entry = NULL;
    uint64_t written_total = MAX_PACKAGE_EXTRACTED_BYTES;

    create_archive(archive_path, sizeof(archive_path));
    file = fopen(archive_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_archive_open_stream(&reader, file));
    TEST_ASSERT_NOT_NULL(reader);
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_read_next_header(reader, &entry));
    TEST_ASSERT_NOT_NULL(entry);

    /* The limit check happens before archive_write_data_block(), so no writer is required here. */
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ARCHIVE_UNSAFE,
        copy_entry_data(reader, NULL, archive_entry_size(entry), &written_total));
    TEST_ASSERT_EQUAL_UINT64(MAX_PACKAGE_EXTRACTED_BYTES, written_total);

    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_read_close(reader));
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_read_free(reader));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_hash_oom_is_resource_failure_before_write(void) {
    char archive_path[CUP_TEST_TEMP_PATH_SIZE];
    char destination[CUP_TEST_TEMP_PATH_SIZE];
    char extracted[CUP_TEST_TEMP_PATH_SIZE];
    char *diagnostic;
    CupError result;

    create_archive(archive_path, sizeof(archive_path));
    join_path(destination, sizeof(destination), root, "out");
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(destination, 0700));
    join_path(extracted, sizeof(extracted), destination, "tool");

    diagnostic = capture_resource_failure(archive_path, destination, &result);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_EXTRACT, result);
    TEST_ASSERT_NOT_NULL(strstr(diagnostic, "failed to allocate archive path state"));
    TEST_ASSERT_NULL(strstr(diagnostic, "collid"));
    TEST_ASSERT_FALSE(test_access_exists(extracted));
    free(diagnostic);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_declared_size_limits_are_owned_by_extraction);
    RUN_TEST(test_actual_decoded_limit_is_rechecked_before_write);
    RUN_TEST(test_hash_oom_is_resource_failure_before_write);
    return UNITY_END();
}
