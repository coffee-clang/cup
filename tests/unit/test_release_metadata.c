/* Exercises canonical release versions and strict format-2 release manifests. */

#include "release_metadata.h"
#include "test_platform.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
#define TEST_RELEASE_PATH_SIZE (CUP_TEST_TEMP_PATH_SIZE + 64)

void setUp(void) {}
void tearDown(void) {}

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

static void write_text(const char *path, const char *text) { write_bytes(path, text, strlen(text)); }

static const char *canonical_manifest =
    "format=2\n"
    "version=0.4.0\n"
    "commit=0123456789abcdef0123456789abcdef01234567\n"
    "root_layout=2\n"
    "catalog_format=1\n"
    "asset_count=4\n"
    "asset.0.name=LICENSE\n"
    "asset.0.sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "asset.1.name=THIRD_PARTY_NOTICES.txt\n"
    "asset.1.sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
    "asset.2.name=catalog.cfg\n"
    "asset.2.sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
    "asset.3.name=cup-linux-x64\n"
    "asset.3.sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n";

static void test_release_version_parse(void) {
    ReleaseVersion version;
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_version_parse("0.4.0", &version));
    TEST_ASSERT_EQUAL_UINT(0, version.major);
    TEST_ASSERT_EQUAL_UINT(4, version.minor);
    TEST_ASSERT_EQUAL_UINT(0, version.patch);
    TEST_ASSERT_EQUAL_INT(CUP_OK, release_version_parse("999999.999999.999999", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("01.2.3", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.3.4", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_version_parse("1.2.03", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_version_parse(NULL, NULL));
}

static void test_load_canonical_manifest(void) {
    ReleaseMetadata metadata;
    const ReleaseAsset *asset;
    char path[TEST_RELEASE_PATH_SIZE];
    release_metadata_init(&metadata);
    build_path(path, sizeof(path), "release.txt");
    write_text(path, canonical_manifest);

    TEST_ASSERT_EQUAL_INT(CUP_OK, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_STRING("0.4.0", metadata.version);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef01234567", metadata.commit);
    TEST_ASSERT_EQUAL_UINT(2, metadata.root_layout);
    TEST_ASSERT_EQUAL_UINT(1, metadata.catalog_format);
    TEST_ASSERT_EQUAL_size_t(4, metadata.asset_count);
    asset = release_metadata_find_asset(&metadata, "cup-linux-x64");
    TEST_ASSERT_NOT_NULL(asset);
    TEST_ASSERT_EQUAL_STRING("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", asset->sha256);
    TEST_ASSERT_NULL(release_metadata_find_asset(&metadata, "release.txt"));
    release_metadata_free(&metadata);
}

static void assert_invalid(const char *name, const char *contents) {
    ReleaseMetadata metadata;
    char path[TEST_RELEASE_PATH_SIZE];
    release_metadata_init(&metadata);
    build_path(path, sizeof(path), name);
    write_text(path, contents);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_size_t(0, metadata.asset_count);
    release_metadata_free(&metadata);
}

static void test_reject_noncanonical_manifest(void) {
    assert_invalid("format1.txt",
                   "format=1\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n");
    assert_invalid("unsorted.txt",
                   "format=2\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n"
                   "root_layout=2\ncatalog_format=1\nasset_count=2\n"
                   "asset.0.name=cup-linux-x64\nasset.0.sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                   "asset.1.name=LICENSE\nasset.1.sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n");
    assert_invalid("self.txt",
                   "format=2\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n"
                   "root_layout=2\ncatalog_format=1\nasset_count=1\n"
                   "asset.0.name=release.txt\nasset.0.sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    assert_invalid("bad-sha.txt",
                   "format=2\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n"
                   "root_layout=2\ncatalog_format=1\nasset_count=1\n"
                   "asset.0.name=LICENSE\nasset.0.sha256=xyz\n");
    assert_invalid("gap.txt",
                   "format=2\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n"
                   "root_layout=2\ncatalog_format=1\nasset_count=1\n"
                   "asset.1.name=LICENSE\nasset.1.sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    assert_invalid("extra.txt",
                   "format=2\nversion=0.4.0\ncommit=0123456789abcdef0123456789abcdef01234567\n"
                   "root_layout=2\ncatalog_format=1\nasset_count=0\nextra=x\n");
}

static void test_load_failures(void) {
    ReleaseMetadata metadata;
    char path[TEST_RELEASE_PATH_SIZE];
    unsigned char *oversized = malloc(CUP_RELEASE_MANIFEST_MAX_BYTES + 1u);
    TEST_ASSERT_NOT_NULL(oversized);
    release_metadata_init(&metadata);
    build_path(path, sizeof(path), "missing.txt");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, release_metadata_load(path, &metadata));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_metadata_load(NULL, &metadata));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, release_metadata_load(path, NULL));
    memset(oversized, 'x', CUP_RELEASE_MANIFEST_MAX_BYTES + 1u);
    build_path(path, sizeof(path), "oversized.txt");
    write_bytes(path, oversized, CUP_RELEASE_MANIFEST_MAX_BYTES + 1u);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, release_metadata_load(path, &metadata));
    free(oversized);
    release_metadata_free(&metadata);
}

int main(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(temp_dir, sizeof(temp_dir), "cup-release-metadata-test"));
    UNITY_BEGIN();
    RUN_TEST(test_release_version_parse);
    RUN_TEST(test_load_canonical_manifest);
    RUN_TEST(test_reject_noncanonical_manifest);
    RUN_TEST(test_load_failures);
    return UNITY_END();
}
