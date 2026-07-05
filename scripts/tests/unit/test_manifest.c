#include "manifest.h"
#include "layout.h"
#include "system.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char temp_dir[] = "/tmp/cup-manifest-test-XXXXXX";

void setUp(void) {}
void tearDown(void) {}

CupError layout_get_manifest_path(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return CUP_ERR_MANIFEST;
}

CupError system_path_exists(const char *path, int *exists) {
    (void)path;
    if (exists != NULL) {
        *exists = 0;
    }
    return CUP_OK;
}

static void build_path(char *out, size_t size, const char *name) {
    TEST_ASSERT_TRUE(snprintf(out, size, "%s/%s", temp_dir, name) > 0);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_valid_manifest(const char *path) {
    write_text(path,
        "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5,21.1.5\n"
        "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.formats=tar.xz,tar.gz,zip\n"
        "compiler.clang.linux-x64.linux-x64.url_template=https://example.invalid/{tool}-{version}-{host_platform}-{target_platform}.{format}\n"
        "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://example.invalid/{tool}-{version}-{host_platform}-{target_platform}/SHA256SUMS\n");
}

static void test_manifest_load_and_query(void) {
    Manifest manifest;
    char path[256];
    char value[MAX_MANIFEST_URL_LEN];
    int flag = 0;

    manifest_init(&manifest);
    build_path(path, sizeof(path), "valid.cfg");
    write_valid_manifest(path);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_load_path(&manifest, path, MANIFEST_SOURCE_DEVELOPMENT));
    TEST_ASSERT_EQUAL_size_t(1, manifest.count);
    TEST_ASSERT_EQUAL_STRING("compiler", manifest.packages[0].component);
    TEST_ASSERT_EQUAL_STRING("clang", manifest.packages[0].tool);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_resolve_stable(&manifest, value, sizeof(value),
            "compiler", "clang", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("22.1.5", value);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_has_version(&manifest, "compiler", "clang", "linux-x64",
            "linux-x64", "21.1.5", &flag));
    TEST_ASSERT_TRUE(flag);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_has_format(&manifest, "compiler", "clang", "linux-x64",
            "linux-x64", "zip", &flag));
    TEST_ASSERT_TRUE(flag);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_build_url(&manifest, value, sizeof(value), "compiler", "clang",
            "linux-x64", "linux-x64", "22.1.5", "tar.xz"));
    TEST_ASSERT_EQUAL_STRING(
        "https://example.invalid/clang-22.1.5-linux-x64-linux-x64.tar.xz",
        value);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
        manifest_build_checksum_url(&manifest, value, sizeof(value), "compiler",
            "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_EQUAL_STRING(
        "https://example.invalid/clang-22.1.5-linux-x64-linux-x64/SHA256SUMS",
        value);

    manifest_free(&manifest);
}

static void test_manifest_rejects_incomplete_duplicate_and_unsafe_url(void) {
    Manifest manifest;
    char path[256];

    manifest_init(&manifest);

    build_path(path, sizeof(path), "missing-checksum.cfg");
    write_text(path,
        "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.url_template=https://example.invalid/{tool}.{format}\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_MANIFEST,
        manifest_load_path(&manifest, path, MANIFEST_SOURCE_DEVELOPMENT));

    build_path(path, sizeof(path), "duplicate.cfg");
    write_text(path,
        "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.stable_version=21.1.5\n"
        "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.url_template=https://example.invalid/{tool}.{format}\n"
        "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://example.invalid/SHA256SUMS\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_MANIFEST,
        manifest_load_path(&manifest, path, MANIFEST_SOURCE_DEVELOPMENT));

    build_path(path, sizeof(path), "http-url.cfg");
    write_text(path,
        "compiler.clang.linux-x64.linux-x64.stable_version=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.available_versions=22.1.5\n"
        "compiler.clang.linux-x64.linux-x64.default_format=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.formats=tar.xz\n"
        "compiler.clang.linux-x64.linux-x64.url_template=http://example.invalid/{tool}.{format}\n"
        "compiler.clang.linux-x64.linux-x64.checksum_url_template=https://example.invalid/SHA256SUMS\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_MANIFEST,
        manifest_load_path(&manifest, path, MANIFEST_SOURCE_DEVELOPMENT));

    manifest_free(&manifest);
}

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_manifest_load_and_query);
    RUN_TEST(test_manifest_rejects_incomplete_duplicate_and_unsafe_url);
    return UNITY_END();
}
