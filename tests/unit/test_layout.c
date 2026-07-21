/*
 * Test focus: Exercises canonical .cup path construction and runtime/CUP-assets directory
 * creation.
 */

#include "error.h"
#include "layout.h"
#include "package.h"
#include "system.h"
#include "unity.h"

void setUp(void);
void tearDown(void);

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Shared fixture state used by the cases in this suite. */

static char temp_dir[] = "/tmp/cup-layout-test-XXXXXX";

/* Test cases grouped by the public contract they exercise. */

static void test_package_paths(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "windows-x64",
                                .version = "22.1.5"};
    char path[1024];
    char expected[1024];

    /* Root and fixed asset paths share the caller's HOME-derived .cup directory. */
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup", temp_dir) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);

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
    TEST_ASSERT_TRUE(strstr(path, "/.cup/config/SHA256SUMS.linux-x64") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/helpers/uninstall.sh") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/bin/cup") != NULL);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_marker_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "/.cup/uninstall.pending") != NULL);

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
    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
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
        TEST_ASSERT_EQUAL_INT(0, chmod(root_path, 0755));
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_runtime(&missing));
        TEST_ASSERT_EQUAL_size_t(1, missing);
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
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

static void test_recovery_paths(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "linux-x64",
                                .version = "22.1.5"};
    char prefix[1024];
    char path[1024];
    int exists;

    TEST_ASSERT_EQUAL_INT(0, setenv("HOME", temp_dir, 1));
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
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    RUN_TEST(test_package_paths);
    RUN_TEST(test_runtime_paths);
    RUN_TEST(test_recovery_paths);
}
