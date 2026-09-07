/* Exercises update asset naming and destination-path resolution. */

#include "unity.h"

#include "assets.h"
#include "layout.h"
#include "text.h"
#include "update_assets.h"

#include <string.h>

static const char long_binary_name[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char long_platform_name[] = "pppppppppppppppppppppppppppppppppppppppp";

CupError assets_binary_asset_name(char *name, size_t size) {
    return text_copy(name, size, long_binary_name);
}

CupError assets_platform_checksums_name(char *name, size_t size) {
    return text_copy(name, size, long_platform_name);
}

static CupError copy_path(char *buffer, size_t size, const char *path) {
    return text_copy(buffer, size, path);
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "/root/bin/cup");
}
CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "/root/SHA256SUMS.linux-x64");
}
CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "/root/config/packages.cfg");
}
CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "/root/config/install.cfg");
}
CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "/root/SHA256SUMS.common");
}

void setUp(void) {}
void tearDown(void) {}

static void test_static_specs(void) {
    UpdateAssetSpec specs[CUP_UPDATE_ASSET_COUNT];

    TEST_ASSERT_EQUAL_INT(CUP_OK, update_asset_specs(specs));
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_BINARY_NEW, specs[0].new_name);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_BINARY_OLD, specs[0].old_name);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_BINARY_ABSENT, specs[0].absent_name);
    TEST_ASSERT_EQUAL_STRING("binary_sha256", specs[0].generation_key);
    TEST_ASSERT_EQUAL_INT(1, specs[0].executable);
    TEST_ASSERT_EQUAL_INT(0, specs[0].read_only);

    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_PLATFORM_CHECKSUMS_NEW, specs[1].new_name);
    TEST_ASSERT_EQUAL_STRING("platform_checksums_sha256", specs[1].generation_key);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_PACKAGES_NEW, specs[2].new_name);
    TEST_ASSERT_EQUAL_STRING("packages_sha256", specs[2].generation_key);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_INSTALL_POLICY_NEW, specs[3].new_name);
    TEST_ASSERT_EQUAL_STRING("install_policy_sha256", specs[3].generation_key);
    TEST_ASSERT_EQUAL_STRING(CUP_UPDATE_COMMON_CHECKSUMS_NEW, specs[4].new_name);
    TEST_ASSERT_EQUAL_STRING("common_checksums_sha256", specs[4].generation_key);
    TEST_ASSERT_EQUAL_INT(1, specs[1].read_only);
    TEST_ASSERT_EQUAL_INT(1, specs[2].read_only);
    TEST_ASSERT_EQUAL_INT(1, specs[3].read_only);
    TEST_ASSERT_EQUAL_INT(1, specs[4].read_only);
}

static void test_destinations(void) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_destination(CUP_UPDATE_ASSET_BINARY, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/root/bin/cup", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_destination(
                              CUP_UPDATE_ASSET_PLATFORM_CHECKSUMS, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/root/SHA256SUMS.linux-x64", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_destination(CUP_UPDATE_ASSET_PACKAGES, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/root/config/packages.cfg", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_destination(
                              CUP_UPDATE_ASSET_INSTALL_POLICY, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/root/config/install.cfg", path);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_destination(
                              CUP_UPDATE_ASSET_COMMON_CHECKSUMS, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/root/SHA256SUMS.common", path);
}

static void test_release_names_use_path_segment_capacity(void) {
    char name[MAX_PATH_SEGMENT_LEN];
    char identifier_sized[MAX_IDENTIFIER_LEN];

    TEST_ASSERT_TRUE(strlen(long_binary_name) >= MAX_IDENTIFIER_LEN);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          update_asset_release_name(
                              CUP_UPDATE_ASSET_BINARY, identifier_sized, sizeof(identifier_sized)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_release_name(CUP_UPDATE_ASSET_BINARY, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING(long_binary_name, name);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        update_asset_release_name(CUP_UPDATE_ASSET_PLATFORM_CHECKSUMS, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING(long_platform_name, name);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          update_asset_release_name(CUP_UPDATE_ASSET_PACKAGES, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING(CUP_PACKAGES_FILENAME, name);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_static_specs);
    RUN_TEST(test_destinations);
    RUN_TEST(test_release_names_use_path_segment_capacity);
    return UNITY_END();
}
