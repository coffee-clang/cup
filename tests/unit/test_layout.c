/*
 * Exercises canonical .cup path construction and runtime/cup-assets directory
 * creation.
 */

#include "constants.h"
#include "download.h"
#include "error.h"
#include "layout.h"
#include "package.h"
#include "path.h"
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

static void create_owned_root(const char *root) {
    char marker[1024];

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_TRUE(snprintf(marker, sizeof(marker), "%s/root.txt", root) > 0);
    write_text_file(marker, "format=1\nproduct=coffee-clang/cup\nlayout=1\n");
}

static void get_root_marker_path_for_test(char *buffer, size_t size) {
    char root[MAX_PATH_LEN];
    int written;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(root, sizeof(root)));
    written = snprintf(buffer, size, "%s/root.txt", root);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
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
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_bin_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/bin", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_components_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/components", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_staging_dir(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/staging", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_state_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/state.txt", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_package_catalog_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/config/packages.cfg", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_install_policy_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/config/install.cfg", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_preferences_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/config/preferences.txt", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_common_checksums_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/config/SHA256SUMS.common", home) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_platform_checksums_path(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, platform_get_host(host, sizeof(host)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/config/SHA256SUMS.%s", home, host) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_uninstall_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/helpers/%s", home, CUP_UNINSTALL_FILENAME) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_binary_path(path, sizeof(path)));
    TEST_ASSERT_TRUE(snprintf(expected, sizeof(expected), "%s/.cup/bin/%s", home, CUP_BINARY_FILENAME) > 0);
    TEST_ASSERT_EQUAL_STRING(expected, path);
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
                          layout_build_staging_prefix(path, sizeof(path), "install", &invalid));
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
    char fallback[1024];
    char path[1024];
    char marker[1024];
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/root-selection", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(
        snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);

    /* An unrelated primary directory is preserved and selects the deterministic fallback. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/unrelated.txt", primary) > 0);
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("foreign\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_TRUE(path_equal(fallback, path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    get_root_marker_path_for_test(marker, sizeof(marker));
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

    /* Once owned, the fallback remains selected even if the primary name is unavailable. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_TRUE(path_equal(fallback, path));
}

#if defined(_WIN32)
static void test_fallback_snapshot_lock_runtime_sequence(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char lock_path[1024];
    char sentinel_path[1024];
    char selected[1024];
    const char *stage = "layout_root_snapshot_begin";
    CupError err;
    SystemLock lock = {0};

    TEST_ASSERT_TRUE(
        snprintf(home, sizeof(home), "%s/fallback-snapshot-lock", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(
        snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(
        snprintf(sentinel_path, sizeof(sentinel_path), "%s/foreign.txt", primary) > 0);
    write_text_file(sentinel_path, "foreign\n");

    /* Match main + repair: freeze a missing fallback, create it, lock it, revalidate the
     * same native identity, re-ensure the root while locked, then initialize runtime dirs. */
    err = layout_root_snapshot_begin();
    if (err == CUP_OK) {
        stage = "layout_get_root";
        err = layout_get_root(selected, sizeof(selected));
    }
    if (err == CUP_OK && !path_equal(fallback, selected)) {
        stage = "fallback selection";
        err = CUP_ERR_INCONSISTENT_STATE;
    }
    if (err == CUP_OK) {
        stage = "layout_ensure_root before lock";
        err = layout_ensure_root();
    }
    if (err == CUP_OK) {
        stage = "layout_get_lock_path";
        err = layout_get_lock_path(lock_path, sizeof(lock_path));
    }
    if (err == CUP_OK) {
        stage = "system_lock_acquire";
        err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    }
    if (err == CUP_OK) {
        stage = "layout_root_snapshot_validate after lock";
        err = layout_root_snapshot_validate();
    }
    if (err == CUP_OK) {
        stage = "layout_ensure_root while locked";
        err = layout_ensure_root();
    }
    if (err == CUP_OK) {
        stage = "layout_ensure_runtime while locked";
        err = layout_ensure_runtime();
    }

    if (lock.active) {
        system_lock_release(&lock);
    }
    layout_root_snapshot_end();
    if (err != CUP_OK) {
        fprintf(stderr, "fallback snapshot/lock/runtime stage failed: %s (%d)\n", stage, err);
    }
    TEST_ASSERT_EQUAL_INT(CUP_OK, err);
}
#endif

static void test_corrupt_owned_root_marker_blocks_fallback(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;
    int exists = 0;
    FILE *file;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/corrupt-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin", primary) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(path));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin/%s", primary, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "cup trace\n");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    {
        static const unsigned char hidden_suffix[] =
            "format=1\nproduct=coffee-clang/cup\nlayout=1\n\0";
        TEST_ASSERT_EQUAL_size_t(sizeof(hidden_suffix) - 1,
                                 fwrite(hidden_suffix, 1, sizeof(hidden_suffix) - 1, file));
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(fallback, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_unmarked_cup_root_blocks_fallback(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;
    int exists = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/unmarked-cup-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    make_child_directory(primary, "bin");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin/%s", primary, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "unmarked cup generation\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(fallback, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_markerless_state_only_directory_uses_fallback(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/state-only-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/state.txt", primary) > 0);
    write_text_file(path, "format=1\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_TRUE(path_equal(fallback, path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(0, issues);
}

static void test_complete_unmarked_current_layout_is_not_adopted(void) {
    char home[1024];
    char primary[1024];
    char path[1024];
    const char *directories[] = {"bin", "components", "staging", "cache", "config", "helpers"};
    size_t i;
    int exists = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/complete-unmarked-root", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    for (i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        make_child_directory(primary, directories[i]);
    }
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/bin/%s", primary, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "current-looking but unmarked\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_ensure_root());
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(path, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_root_candidate_conflicts(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/root-conflicts", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    create_owned_root(primary);
    create_owned_root(fallback);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
}

static void test_both_foreign_roots_are_rejected(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/foreign-roots", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(fallback));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/foreign", primary) > 0);
    write_text_file(path, "foreign\n");
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/foreign", fallback) > 0);
    write_text_file(path, "foreign\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
}

static void test_invalid_marker_blocks_fallback_without_other_traces(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;
    int exists = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/marker-no-traces", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/root.txt", primary) > 0);
    write_text_file(path, "not-cup\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(fallback, &exists));
    TEST_ASSERT_FALSE(exists);
}

static void test_invalid_marker_shape_is_classified(void) {
    char home[1024];
    char primary[1024];
    char fallback[1024];
    char marker_path[1024];
    char selected[1024];
    size_t issues = 0;
    FILE *file;
    size_t i;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/invalid-marker-shape", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(primary, sizeof(primary), "%s/.cup", home) > 0);
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(primary));
    TEST_ASSERT_TRUE(snprintf(marker_path, sizeof(marker_path), "%s/root.txt", primary) > 0);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(marker_path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          layout_get_root(selected, sizeof(selected)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_remove_tree(marker_path, NULL));

    file = fopen(marker_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    for (i = 0; i < 256; ++i) {
        TEST_ASSERT_TRUE(fputs("oversized-marker\n", file) >= 0);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    issues = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          layout_get_root(selected, sizeof(selected)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
    {
        SystemPathKind fallback_kind = SYSTEM_PATH_OTHER;
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(fallback, &fallback_kind));
        TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, fallback_kind);
    }
}

static void test_fallback_unmarked_cup_root_blocks_primary(void) {
    char home[1024];
    char fallback[1024];
    char path[1024];
    size_t issues = 0;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/fallback-unmarked", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_TRUE(snprintf(fallback, sizeof(fallback), "%s/.coffee-cup", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(fallback));
    make_child_directory(fallback, "bin");
    TEST_ASSERT_TRUE(
        snprintf(path, sizeof(path), "%s/bin/%s", fallback, CUP_BINARY_FILENAME) > 0);
    write_text_file(path, "unmarked cup\n");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_get_root(path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_check_root_candidates(&issues));
    TEST_ASSERT_EQUAL_size_t(1, issues);
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
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_runtime_status(&status));
        TEST_ASSERT_EQUAL_INT(LAYOUT_RUNTIME_INCOMPLETE, status);
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

static void test_root_snapshot_is_stable_and_identity_bound(void) {
    char first_home[1024];
    char second_home[1024];
    char first_root[1024];
    char moved_root[1024];
    char selected[1024];

    TEST_ASSERT_TRUE(snprintf(first_home, sizeof(first_home), "%s/snapshot-first", temp_dir) > 0);
    TEST_ASSERT_TRUE(
        snprintf(second_home, sizeof(second_home), "%s/snapshot-second", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(first_home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(second_home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(first_home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_root_snapshot_begin());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(first_root, sizeof(first_root)));
    TEST_ASSERT_TRUE(snprintf(selected, sizeof(selected), "%s/.cup", first_home) > 0);
    TEST_ASSERT_TRUE(path_equal(selected, first_root));

    TEST_ASSERT_EQUAL_INT(0, test_set_home(second_home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(selected, sizeof(selected)));
    TEST_ASSERT_TRUE(path_equal(first_root, selected));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_root());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_root_snapshot_validate());

    TEST_ASSERT_TRUE(snprintf(moved_root, sizeof(moved_root), "%s.moved", first_root) > 0);
    TEST_ASSERT_EQUAL_INT(0, rename(first_root, moved_root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(first_root));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_root_snapshot_validate());
    layout_root_snapshot_end();

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(selected, sizeof(selected)));
    {
        char expected_root[1024];

        TEST_ASSERT_TRUE(snprintf(expected_root, sizeof(expected_root), "%s/.cup", second_home) > 0);
        TEST_ASSERT_TRUE(path_equal(expected_root, selected));
    }
}

static void test_root_snapshot_does_not_adopt_concurrent_creation(void) {
    char home[1024];
    char root[1024];
    char marker_path[1024];
    char sentinel_path[1024];
    SystemPathKind kind;
    FILE *sentinel;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/snapshot-concurrent", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_root_snapshot_begin());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_root(root, sizeof(root)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(root));
    TEST_ASSERT_TRUE(snprintf(sentinel_path, sizeof(sentinel_path), "%s/foreign.txt", root) > 0);
    sentinel = fopen(sentinel_path, "wb");
    TEST_ASSERT_NOT_NULL(sentinel);
    TEST_ASSERT_TRUE(fputs("foreign\n", sentinel) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(sentinel));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, layout_root_snapshot_validate());
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, layout_ensure_root());
    TEST_ASSERT_TRUE(snprintf(marker_path, sizeof(marker_path), "%s/root.txt", root) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(marker_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(sentinel_path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, kind);
    layout_root_snapshot_end();
}

static void test_directory_creation_prevalidates_identity(void) {
    PackageIdentity identity = {.component = "compiler",
                                .tool = "clang",
                                .host_platform = "linux-x64",
                                .target_platform = "linux-x64",
                                .version = "22.1.5"};
    char home[1024];
    char path[1024];
    SystemPathKind kind;

    TEST_ASSERT_TRUE(snprintf(home, sizeof(home), "%s/prevalidate-identity", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(home));
    TEST_ASSERT_EQUAL_INT(0, test_set_home(home));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());

    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/.cup/components/compiler", home) > 0);
    identity.target_platform[0] = '.';
    identity.target_platform[1] = '.';
    identity.target_platform[2] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, layout_ensure_package_parent(&identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);

    TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/.cup/recovery", home) > 0);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          layout_create_recovery_dir((char[1024]){0}, 1024, &identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(path, &kind));
    TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);

#if !defined(_WIN32)
    {
        char long_home[1024];
        char next[1024];
        char segment[48];
        int written;

        written = snprintf(long_home, sizeof(long_home), "%s", temp_dir);
        TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(long_home));
        memset(segment, 'h', sizeof(segment) - 1);
        segment[sizeof(segment) - 1] = '\0';
        while (strlen(long_home) < 945) {
            written = snprintf(next, sizeof(next), "%s/%s", long_home, segment);
            TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(next));
            TEST_ASSERT_EQUAL_INT(CUP_OK, system_make_directory(next));
            written = snprintf(long_home, sizeof(long_home), "%s", next);
            TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(long_home));
        }
        TEST_ASSERT_EQUAL_INT(0, test_set_home(long_home));
        TEST_ASSERT_EQUAL_INT(CUP_OK, layout_ensure_runtime());
        written = snprintf(identity.target_platform,
                           sizeof(identity.target_platform),
                           "%s",
                           "linux-x64");
        TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(identity.target_platform));
        written = snprintf(identity.version,
                           sizeof(identity.version),
                           "%s",
                           "1234567890123456789012345678901");
        TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(identity.version));
        TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, layout_ensure_cache_parent(&identity));
        TEST_ASSERT_TRUE(snprintf(path, sizeof(path), "%s/.cup/cache/compiler", long_home) > 0);
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_get_path_kind(path, &kind));
        TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_MISSING, kind);
    }
#endif
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


void register_layout_tests(void) {
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-layout-test"));
    RUN_TEST(test_package_paths);
    RUN_TEST(test_path_argument_contracts);
    RUN_TEST(test_runtime_paths);
    RUN_TEST(test_runtime_wrong_path_type);
    RUN_TEST(test_directory_creation_prevalidates_identity);
    RUN_TEST(test_recovery_paths);
    RUN_TEST(test_root_snapshot_is_stable_and_identity_bound);
    RUN_TEST(test_root_snapshot_does_not_adopt_concurrent_creation);
    RUN_TEST(test_root_selection);
    RUN_TEST(test_corrupt_owned_root_marker_blocks_fallback);
    RUN_TEST(test_unmarked_cup_root_blocks_fallback);
    RUN_TEST(test_markerless_state_only_directory_uses_fallback);
    RUN_TEST(test_complete_unmarked_current_layout_is_not_adopted);
    RUN_TEST(test_root_candidate_conflicts);
    RUN_TEST(test_both_foreign_roots_are_rejected);
    RUN_TEST(test_invalid_marker_blocks_fallback_without_other_traces);
    RUN_TEST(test_invalid_marker_shape_is_classified);
    RUN_TEST(test_fallback_unmarked_cup_root_blocks_primary);
#if defined(_WIN32)
    RUN_TEST(test_fallback_snapshot_lock_runtime_sequence);
#endif
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(temp_dir));
}
