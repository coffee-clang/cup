#include "generation.h"
#include "system.h"
#include "version.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static ReleaseAsset assets[3];
static SystemPathKind release_kind;
static SystemPathKind license_kind;
static SystemPathKind notices_kind;
static SystemPathKind binary_kind;
static CupError metadata_load_result;
static char metadata_version[CUP_RELEASE_VERSION_MAX];
static const char *digest_mismatch_path;
static int binary_executable;

static void fill_asset(size_t i, const char *name, char digit) {
    memset(&assets[i], 0, sizeof(assets[i]));
    strcpy(assets[i].name, name);
    memset(assets[i].sha256, digit, 64);
    assets[i].sha256[64] = '\0';
}

void setUp(void) {
    fill_asset(0, "LICENSE", 'a');
    fill_asset(1, "THIRD_PARTY_NOTICES.txt", 'b');
    fill_asset(2, "cup-linux-x64", 'c');
    release_kind = SYSTEM_PATH_REGULAR_FILE;
    license_kind = SYSTEM_PATH_REGULAR_FILE;
    notices_kind = SYSTEM_PATH_REGULAR_FILE;
    binary_kind = SYSTEM_PATH_REGULAR_FILE;
    metadata_load_result = CUP_OK;
    strcpy(metadata_version, CUP_VERSION_BASE);
    digest_mismatch_path = NULL;
    binary_executable = 1;
}
void tearDown(void) {}

CupError platform_get_host(char *buffer, size_t size) {
    if (size < sizeof("linux-x64")) return CUP_ERR_BUFFER_TOO_SMALL;
    strcpy(buffer, "linux-x64");
    return CUP_OK;
}
CupError layout_get_root(char *buffer, size_t size) {
    return snprintf(buffer, size, "/root") < (int)size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}
CupError layout_get_binary_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/root/bin/cup") < (int)size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}
const ReleaseAsset *release_metadata_find_asset(const ReleaseMetadata *metadata, const char *name) {
    size_t i;
    for (i = 0; i < metadata->asset_count; ++i) {
        if (strcmp(metadata->assets[i].name, name) == 0) return &metadata->assets[i];
    }
    return NULL;
}
void release_metadata_init(ReleaseMetadata *metadata) {
    memset(metadata, 0, sizeof(*metadata));
}
void release_metadata_free(ReleaseMetadata *metadata) {
    (void)metadata;
}
CupError release_metadata_load(const char *path, ReleaseMetadata *metadata) {
    (void)path;
    if (metadata_load_result != CUP_OK) return metadata_load_result;
    strcpy(metadata->version, metadata_version);
    metadata->root_layout = CUP_ROOT_LAYOUT_FORMAT;
    metadata->catalog_format = CUP_PACKAGE_CATALOG_FORMAT;
    metadata->assets = assets;
    metadata->asset_count = 3;
    return CUP_OK;
}
CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    if (strcmp(path, "/root/release.txt") == 0) *kind = release_kind;
    else if (strcmp(path, "/root/LICENSE") == 0) *kind = license_kind;
    else if (strcmp(path, "/root/THIRD_PARTY_NOTICES.txt") == 0) *kind = notices_kind;
    else if (strcmp(path, "/root/bin/cup") == 0) *kind = binary_kind;
    else return CUP_ERR_FILESYSTEM;
    return CUP_OK;
}
CupError checksum_sha256_file(const char *path, char *digest, size_t size) {
    char digit;
    if (size < 65) return CUP_ERR_BUFFER_TOO_SMALL;
    if (strcmp(path, "/root/LICENSE") == 0) digit = 'a';
    else if (strcmp(path, "/root/THIRD_PARTY_NOTICES.txt") == 0) digit = 'b';
    else if (strcmp(path, "/root/bin/cup") == 0) digit = 'c';
    else return CUP_ERR_FILESYSTEM;
    if (digest_mismatch_path != NULL && strcmp(path, digest_mismatch_path) == 0) digit = 'f';
    memset(digest, digit, 64);
    digest[64] = '\0';
    return CUP_OK;
}
CupError system_is_executable(const char *path, int *executable) {
    (void)path;
    *executable = binary_executable;
    return CUP_OK;
}

static void test_asset_specs(void) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_asset_specs(specs));
    TEST_ASSERT_EQUAL_STRING("release.txt", specs[0].release_name);
    TEST_ASSERT_EQUAL_STRING("/root/release.txt", specs[0].destination);
    TEST_ASSERT_EQUAL_STRING("LICENSE", specs[1].release_name);
    TEST_ASSERT_EQUAL_STRING("/root/LICENSE", specs[1].destination);
    TEST_ASSERT_EQUAL_STRING("THIRD_PARTY_NOTICES.txt", specs[2].release_name);
    TEST_ASSERT_EQUAL_STRING("cup-linux-x64", specs[3].release_name);
    TEST_ASSERT_EQUAL_STRING("/root/bin/cup", specs[3].destination);
    TEST_ASSERT_TRUE(specs[3].executable);
}

static void test_manifest_requirements(void) {
    ReleaseMetadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.root_layout = CUP_ROOT_LAYOUT_FORMAT;
    metadata.catalog_format = CUP_PACKAGE_CATALOG_FORMAT;
    metadata.assets = assets;
    metadata.asset_count = 3;
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_validate_manifest(&metadata));
    TEST_ASSERT_NOT_NULL(generation_manifest_asset(&metadata, CUP_GENERATION_ASSET_BINARY));
    TEST_ASSERT_NULL(generation_manifest_asset(&metadata, CUP_GENERATION_ASSET_RELEASE));

    metadata.asset_count = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, generation_validate_manifest(&metadata));
    metadata.asset_count = 3;
    metadata.root_layout++;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, generation_validate_manifest(&metadata));
}

static void test_inspection_valid(void) {
    GenerationInspection inspection;
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_TRUE(generation_has_installed_assets(&inspection));
    TEST_ASSERT_TRUE(generation_installed_is_valid(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_VALID, inspection.release);
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_VALID, inspection.binary);
}

static void test_inspection_residual_without_manifest(void) {
    GenerationInspection inspection;
    release_kind = SYSTEM_PATH_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_TRUE(generation_has_installed_assets(&inspection));
    TEST_ASSERT_FALSE(generation_installed_is_valid(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_MISSING, inspection.release);
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.license);
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.binary);
}

static void test_inspection_digest_and_executable_failures(void) {
    GenerationInspection inspection;
    digest_mismatch_path = "/root/LICENSE";
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.license);
    TEST_ASSERT_FALSE(generation_installed_is_valid(&inspection));

    setUp();
    binary_executable = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.binary);
    TEST_ASSERT_FALSE(generation_installed_is_valid(&inspection));
}

static void test_inspection_manifest_and_version_failures(void) {
    GenerationInspection inspection;
    metadata_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.release);
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.binary);

    setUp();
    strcpy(metadata_version, "9.9.9");
    TEST_ASSERT_EQUAL_INT(CUP_OK, generation_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_GENERATION_ASSET_INVALID, inspection.release);
    TEST_ASSERT_FALSE(generation_installed_is_valid(&inspection));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_asset_specs);
    RUN_TEST(test_manifest_requirements);
    RUN_TEST(test_inspection_valid);
    RUN_TEST(test_inspection_residual_without_manifest);
    RUN_TEST(test_inspection_digest_and_executable_failures);
    RUN_TEST(test_inspection_manifest_and_version_failures);
    return UNITY_END();
}
