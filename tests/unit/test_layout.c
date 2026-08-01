/*
 * Test focus: Exercises canonical .cup path construction and runtime/CUP-assets directory
 * creation.
 */

#include "checksum.h"
#include "constants.h"
#include "download.h"
#include "error.h"
#include "layout.h"
#include "package.h"
#include "platform.h"
#include "system.h"
#include "unity.h"
#include "test_platform.h"

void setUp(void);
void tearDown(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];

int download_insecure_loopback_is_allowed(const char *url) {
    (void)url;
    return 0;
}

static void write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void make_child_directory(const char *root, const char *name) {
    char path[1024];

    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/%s", root, name) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(path));
}

static void write_checksum_entry(FILE *file, const char *asset, const char *path) {
    char digest[SHA256_HEX_LENGTH + 1];

    TEST_ASSERT_EQUAL_INT(CUP_OK, checksum_sha256_file(path, digest, sizeof(digest)));
    TEST_ASSERT_TRUE(fprintf(file, "%s  %s\n", digest, asset) > 0);
}

static void write_placeholder_checksum_entry(FILE *file,
                                             const char *asset,
                                             char digit) {
    char digest[SHA256_HEX_LENGTH + 1];

    memset(digest, digit, SHA256_HEX_LENGTH);
    digest[SHA256_HEX_LENGTH] = '\0';
    TEST_ASSERT_TRUE(fprintf(file, "%s  %s\n", digest, asset) > 0);
}

static void create_verified_legacy_root(const char *root) {
    char binary[1024];
    char helper[1024];
    char uninstall[1024];
    char catalog[1024];
    char policy[1024];
    char common[1024];
    char platform_checksums[1024];
    char host[MAX_PLATFORM_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
    FILE *file;

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    make_child_directory(root, "bin");
    make_child_directory(root, "components");
    make_child_directory(root, "staging");
    make_child_directory(root, "cache");
    make_child_directory(root, "config");
    make_child_directory(root, "helpers");

    TEST_ASSERT_TRUE(snprintf(binary, sizeof(binary), "%s/bin/%s", root, CUP_BINARY_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(helper,
                              sizeof(helper),
                              "%s/helpers/%s",
                              root,
                              CUP_UPDATE_HELPER_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(uninstall,
                              sizeof(uninstall),
                              "%s/helpers/%s",
                              root,
                              CUP_UNINSTALL_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(catalog,
                              sizeof(catalog),
                              "%s/config/%s",
                              root,
                              CUP_PACKAGES_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(policy,
                              sizeof(policy),
                              "%s/config/%s",
                              root,
                              CUP_INSTALL_POLICY_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(common,
                              sizeof(common),
                              "%s/config/%s",
                              root,
                              CUP_COMMON_CHECKSUMS_FILENAME) > 0);

    write_text_file(binary, "verified-cup-binary\n");
    write_text_file(helper, "verified-cup-binary\n");
    write_text_file(uninstall, "verified-uninstaller\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(binary, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(helper, 1));
    if (CUP_UNINSTALL_EXECUTABLE) {
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_set_executable(uninstall, 1));
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file("config/packages.cfg", catalog));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_copy_file("config/install.cfg", policy));

    file = fopen(common, "wb");
    TEST_ASSERT_NOT_NULL(file);
    write_checksum_entry(file, CUP_PACKAGES_FILENAME, catalog);
    write_checksum_entry(file, CUP_INSTALL_POLICY_FILENAME, policy);
    write_placeholder_checksum_entry(file, CUP_INSTALL_POSIX_FILENAME, '0');
    write_placeholder_checksum_entry(file, CUP_INSTALL_WINDOWS_FILENAME, '1');
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    TEST_ASSERT_EQUAL_INT(CUP_OK, platform_get_host(host, sizeof(host)));
    TEST_ASSERT_TRUE(snprintf(binary_asset,
                              sizeof(binary_asset),
                              strcmp(host, "windows-x64") == 0 ? "cup-%s.exe" : "cup-%s",
                              host) > 0);
    TEST_ASSERT_TRUE(snprintf(platform_checksums,
                              sizeof(platform_checksums),
                              "%s/config/SHA256SUMS.%s",
                              root,
                              host) > 0);
    file = fopen(platform_checksums, "wb");
    TEST_ASSERT_NOT_NULL(file);
    write_checksum_entry(file, binary_asset, binary);
    write_checksum_entry(file, CUP_UNINSTALL_FILENAME, uninstall);
    write_placeholder_checksum_entry(file, CUP_RELEASE_METADATA_FILENAME, '2');
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void refresh_legacy_common_checksums(const char *root) {
    char catalog[1024];
    char policy[1024];
    char common[1024];
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(catalog,
                              sizeof(catalog),
                              "%s/config/%s",
                              root,
                              CUP_PACKAGES_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(policy,
                              sizeof(policy),
                              "%s/config/%s",
                              root,
                              CUP_INSTALL_POLICY_FILENAME) > 0);
    TEST_ASSERT_TRUE(snprintf(common,
                              sizeof(common),
                              "%s/config/%s",
                              root,
                              CUP_COMMON_CHECKSUMS_FILENAME) > 0);
    file = fopen(common, "wb");
    TEST_ASSERT_NOT_NULL(file);
    write_checksum_entry(file, CUP_PACKAGES_FILENAME, catalog);
    write_checksum_entry(file, CUP_INSTALL_POLICY_FILENAME, policy);
    write_placeholder_checksum_entry(file, CUP_INSTALL_POSIX_FILENAME, '0');
    write_placeholder_checksum_entry(file, CUP_INSTALL_WINDOWS_FILENAME, '1');
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* Test cases grouped by the public contract they exercise. */

static void test_package_paths(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "windows-x64",
                                .version = "22.1.5"};
    char path[1024];
    char expected[1024];
    char home[1024];
    char host[64];

    /* Root and fixed asset paths derive from the platform-validated home directory. */
    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_home_dir(home, sizeof(home)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root_marker_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/root.txt") != NULL);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_bin_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/bin") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_components_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/components") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/staging") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_state_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/state.txt") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/config/packages.cfg") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_install_policy_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/config/install.cfg") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_preferences_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/config/preferences.txt") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_common_checksums_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/config/SHA256SUMS.common") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_platform_checksums_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, platform_get_host(host, sizeof(host)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "/.cup/config/SHA256SUMS.%s", host) > 0);
    TEST_ASSERT_TRUE(strstr(path, expected) != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/helpers/" CUP_UNINSTALL_FILENAME) != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/bin/" CUP_BINARY_FILENAME) != NULL);
    /* Package and cache paths derive exclusively from the validated concrete identity. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_build_install_path(path, sizeof(path), &identity));
    TEST_ASSERT_TRUE(strstr(path, "/components/compiler/clang/linux-x64/windows-x64/22.1.5") !=
                     NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_build_cache_archive_path(path, sizeof(path), &identity, "tar.gz"));
    TEST_ASSERT_TRUE(strstr(path, "/cache/compiler/clang/linux-x64/windows-x64/22.1.5/") != NULL);
    TEST_ASSERT_TRUE(strstr(path, "clang-22.1.5-linux-x64-windows-x64.tar.gz") != NULL);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_root(NULL, 1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_install_path(path, sizeof(path), NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_cache_archive_path(path, sizeof(path), &identity, "../bad"));
}

static void test_path_argument_contracts(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "linux-x64",
                                .version = "22.1.5"};
    PackageIdentity invalid = identity;
    char path[1024];
    char tiny[2];

    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_root(path, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, layout_get_root(tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_config_dir(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_config_dir(path, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_get_package_catalog_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_get_install_policy_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_get_preferences_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_get_platform_checksums_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_get_cup_update_helper_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_uninstall_path(NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_binary_path(NULL, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_install_path(NULL, sizeof(path), &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_install_path(path, 0, &identity));
    invalid.component[0] = '.';
    invalid.component[1] = '.';
    invalid.component[2] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_install_path(path, sizeof(path), &invalid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_cache_archive_path(NULL,
                                                          sizeof(path),
                                                          &identity,
                                                          "tar.xz"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_cache_archive_path(path, 0, &identity, "tar.xz"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_cache_archive_path(path, sizeof(path), &identity, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_cache_archive_path(path, sizeof(path), NULL, "tar.xz"));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_staging_prefix(NULL, sizeof(path), "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_staging_prefix(path, 0, "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_staging_prefix(path, sizeof(path), NULL, &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_staging_prefix(path, sizeof(path), "install", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_staging_dir(NULL, sizeof(path), "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_staging_dir(path, 0, "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_make_staging_path(NULL, sizeof(path), "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_make_staging_path(path, 0, "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_recovery_dir(NULL, sizeof(path), &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_recovery_dir(path, 0, &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_check_root_candidates(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_ensure_cache_parent(NULL));
}

static void test_root_selection(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    char marker[1024];
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/root-selection", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(
        snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);

    /* An unrelated primary directory is preserved and selects the deterministic fallback. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/unrelated.txt", primary) > 0);
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("foreign\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(alternative, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root_marker_path(marker, sizeof(marker)));
    TEST_ASSERT_TRUE(strstr(marker, "/.coffee-cup/root.txt") != NULL);
    file = fopen(marker, "rb");
    TEST_ASSERT_NOT_NULL(file);
    {
        char contents[128];
        size_t count = fread(contents, 1, sizeof(contents) - 1, file);

        contents[count] = '\0';
        TEST_ASSERT_EQUAL_STRING(
            "format=1\nproduct=coffee-clang/cup\nlayout=1\n", contents);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    /* Once owned, the alternative remains selected even if the primary name is unavailable. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(alternative, path);
}

static void test_corrupt_owned_root_marker_blocks_fallback(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;
    int exists = 0;
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/corrupt-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin", primary) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(path));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin/%s", primary, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "cup trace\n");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("corrupt\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(alternative, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_damaged_legacy_root_blocks_fallback(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;
    int exists = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/damaged-legacy-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    make_child_directory(primary, "bin");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin/%s", primary, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "incomplete legacy cup\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(alternative, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_markerless_state_only_directory_uses_fallback(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/state-only-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/state.txt", primary) > 0);
    write_text_file(path, "format=1\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(alternative, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(0, issues);
}

static void test_verified_legacy_root_is_adopted(void) {
    char home[1024];
    char primary[1024];
    char path[1024];
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/verified-legacy-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    create_verified_legacy_root(primary);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(primary, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    {
        char contents[128];
        size_t count = fread(contents, 1, sizeof(contents) - 1, file);

        contents[count] = '\0';
        TEST_ASSERT_EQUAL_STRING(
            "format=1\nproduct=coffee-clang/cup\nlayout=1\n", contents);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_root_candidate_conflicts(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/root-conflicts", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    create_verified_legacy_root(primary);
    create_verified_legacy_root(alternative);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
}

static void test_both_foreign_roots_are_rejected(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/foreign-roots", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(alternative));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/foreign", primary) > 0);
    write_text_file(path, "foreign\n");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/foreign", alternative) > 0);
    write_text_file(path, "foreign\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
}

static void test_invalid_marker_without_cup_traces_uses_fallback(void) {
    char home[1024];
    char primary[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/marker-no-traces", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    write_text_file(path, "not-cup\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(alternative, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(0, issues);
}

static void test_alternative_damaged_legacy_blocks_primary(void) {
    char home[1024];
    char alternative[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/alternative-damaged", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(alternative, sizeof(alternative), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(alternative));
    make_child_directory(alternative, "bin");
    TEST_ASSERT_TRUE(
        snprintf(path, sizeof(path), "%s/bin/%s", alternative, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "incomplete cup\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
}

static void test_legacy_root_rejects_tampering_and_invalid_state(void) {
    char home[1024];
    char primary[1024];
    char path[1024];

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/legacy-tampered", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    create_verified_legacy_root(primary);
    TEST_ASSERT_TRUE(snprintf(path,
                              sizeof(path),
                              "%s/helpers/%s",
                              primary,
                              CUP_UPDATE_HELPER_FILENAME) > 0);
    write_text_file(path, "different helper\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(CUP_OK, test_remove_tree(primary));
    create_verified_legacy_root(primary);
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/state.txt", primary) > 0);
    write_text_file(path, "format=1\ninvalid\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
}

static void test_legacy_root_rejects_invalid_verified_documents(void) {
    char home[1024];
    char primary[1024];
    char path[1024];

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/legacy-invalid-docs", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    create_verified_legacy_root(primary);
    TEST_ASSERT_TRUE(snprintf(path,
                              sizeof(path),
                              "%s/config/%s",
                              primary,
                              CUP_PACKAGES_FILENAME) > 0);
    write_text_file(path, "format=1\ninvalid\n");
    refresh_legacy_common_checksums(primary);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(CUP_OK, test_remove_tree(primary));
    create_verified_legacy_root(primary);
    TEST_ASSERT_TRUE(snprintf(path,
                              sizeof(path),
                              "%s/config/%s",
                              primary,
                              CUP_INSTALL_POLICY_FILENAME) > 0);
    write_text_file(path, "format=1\ninvalid\n");
    refresh_legacy_common_checksums(primary);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
}

static void test_runtime_paths(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "linux-x64",
                                .version = "22.1.5"};
    LayoutRuntimeStatus status;
    char path[1024];
    char state_path[1024];
    size_t missing;
    FILE *file;
    int exists;

    /* Runtime status advances from missing to incomplete and then ready. */
    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_runtime_status(&status));
    TEST_ASSERT_EQUAL_INT(LAYOUT_RUNTIME_MISSING, status);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_runtime(&missing));
    TEST_ASSERT_EQUAL_size_t(4, missing);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_runtime_status(&status));
    TEST_ASSERT_EQUAL_INT(LAYOUT_RUNTIME_INCOMPLETE, status);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_state_path(state_path, sizeof(state_path)));
    file = fopen(state_path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_runtime_status(&status));
    TEST_ASSERT_EQUAL_INT(LAYOUT_RUNTIME_READY, status);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_runtime(&missing));
    TEST_ASSERT_EQUAL_size_t(0, missing);
    /* Root permissions are part of readiness and are repaired idempotently. */
    {
        char root_path[1024];
        int is_private;

        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(root_path, sizeof(root_path)));
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(root_path, &is_private));
        TEST_ASSERT_TRUE(is_private);
#if defined(_WIN32)
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
#else
        TEST_ASSERT_EQUAL_INT(0, chmod(root_path, 0755));
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_runtime(&missing));
        TEST_ASSERT_EQUAL_size_t(1, missing);
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
#endif
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_directory_is_private(root_path, &is_private));
        TEST_ASSERT_TRUE(is_private);
    }

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_config());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_config_dir(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(path, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cup_assets());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_package_parent(&identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_cache_parent(&identity));

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_build_install_path(path, sizeof(path), &identity));
    {
        char *slash = strrchr(path, '/');
        TEST_ASSERT_NOT_NULL(slash);
        *slash = '\0';
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(path, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_get_runtime_status(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_check_runtime(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_ensure_package_parent(NULL));
}

static void test_runtime_wrong_path_type(void) {
    char home[1024];
    char path[1024];
    LayoutRuntimeStatus status;
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/runtime-wrong-type", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_components_dir(path, sizeof(path)));
    write_text_file(path, "not a directory\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_runtime_status(&status));
    TEST_ASSERT_EQUAL_INT(LAYOUT_RUNTIME_INCOMPLETE, status);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_runtime(&issues));
    TEST_ASSERT_TRUE(issues > 0);
}

static void test_recovery_paths(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "linux-x64",
                                .version = "22.1.5"};
    char prefix[1024];
    char path[1024];
    int exists;

    TEST_ASSERT_EQUAL_INT(0, test_set_home(temp_dir));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_staging_prefix(prefix, sizeof(prefix), "install", &identity));
    TEST_ASSERT_EQUAL_STRING("install-compiler-clang-linux-x64-linux-x64-22.1.5", prefix);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_create_staging_dir(path, sizeof(path), "install", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(path, &exists));
    TEST_ASSERT_TRUE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_make_staging_path(path, sizeof(path), "remove", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_create_recovery_dir(path, sizeof(path), &identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_directory(path, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_TRUE(strstr(path, "/.cup/recovery/invalid-compiler-clang-") != NULL);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_build_staging_prefix(prefix, sizeof(prefix), "../bad", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_staging_dir(path, sizeof(path), "../bad", &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_recovery_dir(path, sizeof(path), NULL));
}

/* Suite registration. */

void register_layout_tests(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-layout-test"));
    RUN_TEST(test_package_paths);
    RUN_TEST(test_path_argument_contracts);
    RUN_TEST(test_runtime_paths);
    RUN_TEST(test_runtime_wrong_path_type);
    RUN_TEST(test_recovery_paths);
    RUN_TEST(test_root_selection);
    RUN_TEST(test_corrupt_owned_root_marker_blocks_fallback);
    RUN_TEST(test_damaged_legacy_root_blocks_fallback);
    RUN_TEST(test_markerless_state_only_directory_uses_fallback);
    RUN_TEST(test_verified_legacy_root_is_adopted);
    RUN_TEST(test_root_candidate_conflicts);
    RUN_TEST(test_both_foreign_roots_are_rejected);
    RUN_TEST(test_invalid_marker_without_cup_traces_uses_fallback);
    RUN_TEST(test_alternative_damaged_legacy_blocks_primary);
    RUN_TEST(test_legacy_root_rejects_tampering_and_invalid_state);
    RUN_TEST(test_legacy_root_rejects_invalid_verified_documents);
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(temp_dir));
}
