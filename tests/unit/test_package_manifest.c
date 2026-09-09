/*
 * Exercises the producer-owned manifest.txt format=2 contract against real
 * package filesystem objects without involving installation transactions.
 */

#include "checksum.h"
#include "package_manifest.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define TEST_MANIFEST_HOST "windows-x64"
#define TEST_TOOL_NAME "tool.cmd"
#define TEST_TOOL_BODY "@echo off\r\nexit /b 0\r\n"
#else
#define TEST_MANIFEST_HOST "linux-x64"
#define TEST_TOOL_NAME "tool"
#define TEST_TOOL_BODY "#!/bin/sh\nexit 0\n"
#endif

static char root[CUP_TEST_TEMP_PATH_SIZE];

static void join_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(test_mkdir(path, 0755) == 0 || errno == EEXIST);
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
#endif
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void file_digest(const char *path, char digest[65]) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, 65));
}

#if !defined(_WIN32)
static void text_digest(const char *text, char digest[65]) {
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        checksum_sha256_bytes((const unsigned char *)text, strlen(text), digest, 65));
}
#endif

static void create_basic_tree(char *tool_path, size_t tool_path_size) {
    char bin[1024];
    char info[1024];

    join_path(bin, sizeof(bin), root, "bin");
    make_dir(bin);
    join_path(tool_path, tool_path_size, bin, TEST_TOOL_NAME);
    write_text(tool_path, TEST_TOOL_BODY);
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, chmod(tool_path, 0755));
#endif
    join_path(info, sizeof(info), root, "info.txt");
    write_text(info, "package fixture\n");
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, chmod(info, 0644));
#endif
}

static void write_basic_manifest(void) {
    char tool[1024];
    char info[1024];
    char manifest[1024];
    char tool_digest[65];
    char info_digest[65];
    char body[4096];
    int written;

    join_path(tool, sizeof(tool), root, "bin/" TEST_TOOL_NAME);
    join_path(info, sizeof(info), root, "info.txt");
    join_path(manifest, sizeof(manifest), root, "manifest.txt");
    file_digest(tool, tool_digest);
    file_digest(info, info_digest);
    written = snprintf(body,
                       sizeof(body),
                       "format=2\n"
                       "d\t0755\t-\tbin\n"
                       "f\t0755\t%s\tbin/%s\n"
                       "f\t0644\t%s\tinfo.txt\n",
                       tool_digest,
                       TEST_TOOL_NAME,
                       info_digest);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(body));
    write_text(manifest, body);
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, chmod(manifest, 0644));
#endif
}

void setUp(void) {
    TEST_ASSERT_NOT_NULL(
        test_make_temp_directory(root, sizeof(root), "cup-package-manifest-test"));
}

void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(root));
}

static void test_valid_manifest(void) {
    char tool[1024];

    create_basic_tree(tool, sizeof(tool));
    write_basic_manifest();
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_manifest_verify(root, TEST_MANIFEST_HOST, stderr));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_manifest_verify(NULL, TEST_MANIFEST_HOST, stderr));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_manifest_verify(root, NULL, stderr));
}

static void test_manifest_is_required_and_format_is_strict(void) {
    char tool[1024];
    char manifest[1024];

    create_basic_tree(tool, sizeof(tool));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));

    join_path(manifest, sizeof(manifest), root, "manifest.txt");
    write_text(manifest, "format=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

static void test_payload_changes_are_detected(void) {
    char tool[1024];
    char extra[1024];

    create_basic_tree(tool, sizeof(tool));
    write_basic_manifest();
    write_text(tool, "changed\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));

    create_basic_tree(tool, sizeof(tool));
    write_basic_manifest();
    join_path(extra, sizeof(extra), root, "extra.txt");
    write_text(extra, "undeclared\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

static void test_missing_declared_object_is_detected(void) {
    char tool[1024];

    create_basic_tree(tool, sizeof(tool));
    write_basic_manifest();
    TEST_ASSERT_EQUAL_INT(0, test_unlink(tool));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

static void test_case_fold_path_collisions_are_rejected(void) {
    char upper[1024];
    char lower[1024];
    char manifest[1024];
    char upper_digest[65];
    char lower_digest[65];
    char body[4096];
    int written;

    join_path(upper, sizeof(upper), root, "A");
    join_path(lower, sizeof(lower), root, "a");
    join_path(manifest, sizeof(manifest), root, "manifest.txt");
    write_text(upper, "upper\n");
    write_text(lower, "lower\n");
    file_digest(upper, upper_digest);
    file_digest(lower, lower_digest);
    written = snprintf(body,
                       sizeof(body),
                       "format=2\n"
                       "f\t0644\t%s\tA\n"
                       "f\t0644\t%s\ta\n",
                       upper_digest,
                       lower_digest);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(body));
    write_text(manifest, body);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

static void test_manifest_entries_must_be_sorted(void) {
    char tool[1024];
    char info[1024];
    char manifest[1024];
    char tool_digest[65];
    char info_digest[65];
    char body[4096];
    int written;

    create_basic_tree(tool, sizeof(tool));
    join_path(info, sizeof(info), root, "info.txt");
    join_path(manifest, sizeof(manifest), root, "manifest.txt");
    file_digest(tool, tool_digest);
    file_digest(info, info_digest);
    written = snprintf(body,
                       sizeof(body),
                       "format=2\n"
                       "f\t0644\t%s\tinfo.txt\n"
                       "d\t0755\t-\tbin\n"
                       "f\t0755\t%s\tbin/%s\n",
                       info_digest,
                       tool_digest,
                       TEST_TOOL_NAME);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(body));
    write_text(manifest, body);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

#if !defined(_WIN32)
static void test_posix_modes_are_part_of_the_contract(void) {
    char tool[1024];
    char bin[1024];

    create_basic_tree(tool, sizeof(tool));
    write_basic_manifest();
    join_path(bin, sizeof(bin), root, "bin");
    TEST_ASSERT_EQUAL_INT(0, chmod(bin, 0700));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
}

static void write_symlink_manifest(const char *target_text) {
    char target[1024];
    char info[1024];
    char manifest[1024];
    char target_digest[65];
    char info_digest[65];
    char link_digest[65];
    char body[4096];
    int written;

    join_path(target, sizeof(target), root, "bin/tool-real");
    join_path(info, sizeof(info), root, "info.txt");
    join_path(manifest, sizeof(manifest), root, "manifest.txt");
    file_digest(target, target_digest);
    file_digest(info, info_digest);
    text_digest(target_text, link_digest);
    written = snprintf(body,
                       sizeof(body),
                       "format=2\n"
                       "d\t0755\t-\tbin\n"
                       "l\t-\t%s\tbin/tool\n"
                       "f\t0755\t%s\tbin/tool-real\n"
                       "f\t0644\t%s\tinfo.txt\n",
                       link_digest,
                       target_digest,
                       info_digest);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(body));
    write_text(manifest, body);
    TEST_ASSERT_EQUAL_INT(0, chmod(manifest, 0644));
}

static void test_posix_internal_symlink_is_verified(void) {
    char bin[1024];
    char target[1024];
    char link[1024];
    char info[1024];
    char external[1024];

    join_path(bin, sizeof(bin), root, "bin");
    make_dir(bin);
    join_path(target, sizeof(target), bin, "tool-real");
    write_text(target, TEST_TOOL_BODY);
    TEST_ASSERT_EQUAL_INT(0, chmod(target, 0755));
    join_path(link, sizeof(link), bin, "tool");
    TEST_ASSERT_EQUAL_INT(0, symlink("tool-real", link));
    join_path(info, sizeof(info), root, "info.txt");
    write_text(info, "package fixture\n");
    TEST_ASSERT_EQUAL_INT(0, chmod(info, 0644));
    write_symlink_manifest("tool-real");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));

    TEST_ASSERT_EQUAL_INT(0, test_unlink(link));
    join_path(external, sizeof(external), root, "../external-tool");
    write_text(external, "external\n");
    TEST_ASSERT_EQUAL_INT(0, symlink("../../external-tool", link));
    write_symlink_manifest("../../external-tool");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_manifest_verify(root, TEST_MANIFEST_HOST, NULL));
    TEST_ASSERT_EQUAL_INT(0, test_unlink(external));
}
#endif

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_manifest);
    RUN_TEST(test_manifest_is_required_and_format_is_strict);
    RUN_TEST(test_payload_changes_are_detected);
    RUN_TEST(test_missing_declared_object_is_detected);
    RUN_TEST(test_case_fold_path_collisions_are_rejected);
    RUN_TEST(test_manifest_entries_must_be_sorted);
#if !defined(_WIN32)
    RUN_TEST(test_posix_modes_are_part_of_the_contract);
    RUN_TEST(test_posix_internal_symlink_is_verified);
#endif
    return UNITY_END();
}
