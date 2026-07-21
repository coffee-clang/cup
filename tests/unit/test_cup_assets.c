/*
 * Test focus: Exercises CUP asset inspection, source selection, checksum lookup and
 * pending-uninstall detection through controlled filesystem/system boundaries.
 */

#include "cup_assets.h"
#include "checksum.h"
#include "constants.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "platform.h"
#include "system.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static int layout_failure;
static CupError kind_result;
static SystemPathKind binary_kind;
static SystemPathKind helper_kind;
static SystemPathKind package_catalog_kind;
static SystemPathKind install_policy_kind;
static SystemPathKind uninstall_kind;
static SystemPathKind common_kind;
static SystemPathKind platform_kind;
static SystemPathKind marker_kind;
static CupError common_schema_result;
static CupError platform_schema_result;
static CupError executable_result;
static int binary_executable;
static int helper_executable;
static int uninstall_executable;
static CupError regular_result;
static int development_uninstall_regular;
static int development_uninstall_executable;
static CupError installed_catalog_result;
static CupError development_catalog_result;
static CupError installed_install_policy_result;
static CupError development_install_policy_result;
static CupError verify_result;
static int binary_matches;
static int package_catalog_matches;
static int install_policy_matches;
static int uninstall_matches;
static CupError host_result;
static char host_value[MAX_PLATFORM_LEN];

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    layout_failure = 0;
    kind_result = CUP_OK;
    binary_kind = SYSTEM_PATH_MISSING;
    helper_kind = SYSTEM_PATH_MISSING;
    package_catalog_kind = SYSTEM_PATH_MISSING;
    install_policy_kind = SYSTEM_PATH_MISSING;
    uninstall_kind = SYSTEM_PATH_MISSING;
    common_kind = SYSTEM_PATH_MISSING;
    platform_kind = SYSTEM_PATH_MISSING;
    marker_kind = SYSTEM_PATH_MISSING;
    common_schema_result = CUP_OK;
    platform_schema_result = CUP_OK;
    executable_result = CUP_OK;
    binary_executable = 1;
    helper_executable = 1;
    uninstall_executable = 1;
    regular_result = CUP_OK;
    development_uninstall_regular = 0;
    development_uninstall_executable = 1;
    installed_catalog_result = CUP_OK;
    development_catalog_result = CUP_ERR_CATALOG;
    installed_install_policy_result = CUP_OK;
    development_install_policy_result = CUP_ERR_VALIDATION;
    verify_result = CUP_OK;
    binary_matches = 1;
    package_catalog_matches = 1;
    install_policy_matches = 1;
    uninstall_matches = 1;
    host_result = CUP_OK;
    strcpy(host_value, "linux-x64");
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

static CupError write_layout_path(int id, char *buffer, size_t size, const char *value) {
    if (layout_failure == id) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", value), size);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_binary_path(char *buffer, size_t size) {
    return write_layout_path(1, buffer, size, "/binary");
}

CupError layout_get_cup_update_helper_path(char *buffer, size_t size) {
    return write_layout_path(8, buffer, size, "/helper");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return write_layout_path(2, buffer, size, "/catalog");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return write_layout_path(3, buffer, size, "/install-config");
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return write_layout_path(4, buffer, size, "/uninstall");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return write_layout_path(5, buffer, size, "/common");
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return write_layout_path(6, buffer, size, "/platform");
}

CupError layout_get_uninstall_marker_path(char *buffer, size_t size) {
    return write_layout_path(7, buffer, size, "/marker");
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(kind);
    if (kind_result != CUP_OK) {
        return kind_result;
    }
    if (strcmp(path, "/binary") == 0) {
        *kind = binary_kind;
    } else if (strcmp(path, "/helper") == 0) {
        *kind = helper_kind;
    } else if (strcmp(path, "/catalog") == 0) {
        *kind = package_catalog_kind;
    } else if (strcmp(path, "/install-config") == 0) {
        *kind = install_policy_kind;
    } else if (strcmp(path, "/uninstall") == 0) {
        *kind = uninstall_kind;
    } else if (strcmp(path, "/common") == 0) {
        *kind = common_kind;
    } else if (strcmp(path, "/platform") == 0) {
        *kind = platform_kind;
    } else if (strcmp(path, "/marker") == 0) {
        *kind = marker_kind;
    } else {
        TEST_FAIL_MESSAGE("unexpected CUP assets path");
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

CupError checksum_validate_assets(const char *checksum_path,
                                  const char *const *asset_names,
                                  size_t asset_count) {
    TEST_ASSERT_NOT_NULL(checksum_path);
    TEST_ASSERT_NOT_NULL(asset_names);
    TEST_ASSERT_TRUE(asset_count >= 1);
    if (strcmp(checksum_path, "/common") == 0) {
        TEST_ASSERT_EQUAL_UINT(2, asset_count);
        TEST_ASSERT_EQUAL_STRING(CUP_PACKAGES_FILENAME, asset_names[0]);
        TEST_ASSERT_EQUAL_STRING(CUP_INSTALL_POLICY_FILENAME, asset_names[1]);
        return common_schema_result;
    }
    TEST_ASSERT_EQUAL_UINT(3, asset_count);
    return platform_schema_result;
}

CupError system_is_executable(const char *path, int *is_executable) {
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(is_executable);
    if (executable_result != CUP_OK) {
        return executable_result;
    }
    if (strcmp(path, "/binary") == 0) {
        *is_executable = binary_executable;
    } else if (strcmp(path, "/helper") == 0) {
        *is_executable = helper_executable;
    } else if (strcmp(path, "/uninstall") == 0) {
        *is_executable = uninstall_executable;
    } else if (strcmp(path, CUP_DEVELOPMENT_UNINSTALL_PATH) == 0) {
        *is_executable = development_uninstall_executable;
    } else {
        TEST_FAIL_MESSAGE("unexpected executable path");
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

void package_catalog_init(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    memset(catalog, 0, sizeof(*catalog));
}

void package_catalog_free(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    return installed_catalog_result;
}

CupError package_catalog_load_development(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    return development_catalog_result;
}

CupError package_catalog_load_path(PackageCatalog *catalog,
                                   const char *path,
                                   PackageCatalogSource source) {
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_EQUAL_STRING("/catalog", path);
    TEST_ASSERT_EQUAL_INT(PACKAGE_CATALOG_SOURCE_INSTALLED, source);
    return installed_catalog_result;
}

void install_policy_init(InstallPolicy *config) {
    TEST_ASSERT_NOT_NULL(config);
    memset(config, 0, sizeof(*config));
}

CupError install_policy_load_path(InstallPolicy *config,
                                  const char *path,
                                  InstallPolicySource source) {
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_EQUAL_STRING("/install-config", path);
    TEST_ASSERT_EQUAL_INT(INSTALL_POLICY_SOURCE_INSTALLED, source);
    return installed_install_policy_result;
}

CupError install_policy_load_development(InstallPolicy *config) {
    TEST_ASSERT_NOT_NULL(config);
    return development_install_policy_result;
}

CupError checksum_verify_file(const char *checksum_path,
                              const char *asset_name,
                              const char *asset_path,
                              int *matches) {
    TEST_ASSERT_NOT_NULL(checksum_path);
    TEST_ASSERT_NOT_NULL(asset_name);
    TEST_ASSERT_NOT_NULL(asset_path);
    TEST_ASSERT_NOT_NULL(matches);
    if (verify_result != CUP_OK) {
        return verify_result;
    }
    if (strcmp(asset_path, "/binary") == 0) {
        *matches = binary_matches;
    } else if (strcmp(asset_path, "/catalog") == 0) {
        *matches = package_catalog_matches;
    } else if (strcmp(asset_path, "/install-config") == 0) {
        *matches = install_policy_matches;
    } else if (strcmp(asset_path, "/uninstall") == 0) {
        *matches = uninstall_matches;
    } else {
        TEST_FAIL_MESSAGE("unexpected checksum asset path");
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

CupError system_is_regular_file(const char *path, int *is_regular_file) {
    TEST_ASSERT_EQUAL_STRING(CUP_DEVELOPMENT_UNINSTALL_PATH, path);
    TEST_ASSERT_NOT_NULL(is_regular_file);
    if (regular_result != CUP_OK) {
        return regular_result;
    }
    *is_regular_file = development_uninstall_regular;
    return CUP_OK;
}

CupError platform_get_host(char *buffer, size_t size) {
    if (host_result != CUP_OK) {
        return host_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", host_value), size);
}

static void make_assets_regular(void) {
    binary_kind = SYSTEM_PATH_REGULAR_FILE;
    helper_kind = SYSTEM_PATH_REGULAR_FILE;
    package_catalog_kind = SYSTEM_PATH_REGULAR_FILE;
    install_policy_kind = SYSTEM_PATH_REGULAR_FILE;
    uninstall_kind = SYSTEM_PATH_REGULAR_FILE;
    common_kind = SYSTEM_PATH_REGULAR_FILE;
    platform_kind = SYSTEM_PATH_REGULAR_FILE;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_empty_and_complete(void) {
    CupAssetsInspection inspection;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_assets_inspect(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_FALSE(cup_assets_has_installed_assets(&inspection));
    TEST_ASSERT_FALSE(cup_assets_installed_is_valid(&inspection));
    TEST_ASSERT_FALSE(cup_assets_development_is_valid(&inspection));
    TEST_ASSERT_FALSE(cup_assets_has_installed_assets(NULL));
    TEST_ASSERT_FALSE(cup_assets_installed_is_valid(NULL));
    TEST_ASSERT_FALSE(cup_assets_development_is_valid(NULL));

    reset_scenario();
    make_assets_regular();
    development_catalog_result = CUP_OK;
    development_install_policy_result = CUP_OK;
    development_uninstall_regular = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_TRUE(cup_assets_has_installed_assets(&inspection));
    TEST_ASSERT_TRUE(cup_assets_installed_is_valid(&inspection));
    TEST_ASSERT_TRUE(cup_assets_development_is_valid(&inspection));
}

static void test_bad_assets(void) {
    CupAssetsInspection inspection;

    /* Invalid common and platform checksum schemas invalidate their dependent assets. */
    make_assets_regular();
    common_schema_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.common_checksums);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.catalog);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.install_policy);

    reset_scenario();
    make_assets_regular();
    platform_schema_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.platform_checksums);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.binary);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.uninstall);

    /* Executability requirements differ only where Windows uses script-file semantics. */
    reset_scenario();
    make_assets_regular();
    binary_executable = 0;
    uninstall_executable = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.binary);
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.uninstall);
#else
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_VALID, inspection.uninstall);
#endif

    reset_scenario();
    make_assets_regular();
    helper_executable = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.helper);
#else
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_VALID, inspection.helper);
#endif

    reset_scenario();
    make_assets_regular();
    helper_kind = SYSTEM_PATH_DIRECTORY;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.helper);

    /* Digest and parser failures are reported on the specific installed asset. */
    reset_scenario();
    make_assets_regular();
    binary_matches = 0;
    package_catalog_matches = 0;
    install_policy_matches = 0;
    uninstall_matches = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.binary);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.catalog);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.install_policy);
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.uninstall);

    reset_scenario();
    make_assets_regular();
    installed_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.catalog);

    reset_scenario();
    make_assets_regular();
    installed_install_policy_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_EQUAL_INT(CUP_ASSET_INVALID, inspection.install_policy);
}

static void test_inspection_errors(void) {
    CupAssetsInspection inspection;

    /* Path construction and filesystem errors stop inspection immediately. */
    layout_failure = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_assets_inspect(&inspection));

    reset_scenario();
    layout_failure = 8;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_assets_inspect(&inspection));

    reset_scenario();
    kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));

    reset_scenario();
    common_kind = SYSTEM_PATH_REGULAR_FILE;
    common_schema_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));

    reset_scenario();
    make_assets_regular();
    executable_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));

    reset_scenario();
    make_assets_regular();
    verify_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));

    /* Installed and development parser errors propagate unless the fallback is optional. */
    reset_scenario();
    make_assets_regular();
    installed_catalog_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, cup_assets_inspect(&inspection));

    reset_scenario();
    development_catalog_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, cup_assets_inspect(&inspection));

    reset_scenario();
    development_install_policy_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, cup_assets_inspect(&inspection));

    reset_scenario();
    development_install_policy_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
    TEST_ASSERT_FALSE(inspection.development_install_policy_valid);

    reset_scenario();
    regular_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));

    reset_scenario();
    development_uninstall_regular = 1;
    executable_result = CUP_ERR_FILESYSTEM;
#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_inspect(&inspection));
#else
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_inspect(&inspection));
#endif
}

static void test_uninstall_fallback(void) {
    CupAssetsSource source;
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_assets_find_uninstall(NULL, sizeof(path), &source));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_assets_find_uninstall(path, 0, &source));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          cup_assets_find_uninstall(path, sizeof(path), NULL));

    uninstall_kind = SYSTEM_PATH_REGULAR_FILE;
    platform_kind = SYSTEM_PATH_REGULAR_FILE;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_find_uninstall(path, sizeof(path), &source));
    TEST_ASSERT_EQUAL_INT(CUP_ASSETS_SOURCE_INSTALLED, source);
    TEST_ASSERT_EQUAL_STRING("/uninstall", path);

    reset_scenario();
    development_uninstall_regular = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_find_uninstall(path, sizeof(path), &source));
    TEST_ASSERT_EQUAL_INT(CUP_ASSETS_SOURCE_DEVELOPMENT, source);
    TEST_ASSERT_EQUAL_STRING(CUP_DEVELOPMENT_UNINSTALL_PATH, path);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_assets_find_uninstall(path, sizeof(path), &source));
    TEST_ASSERT_EQUAL_INT(CUP_ASSETS_SOURCE_NONE, source);

    reset_scenario();
    kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          cup_assets_find_uninstall(path, sizeof(path), &source));
}

static void test_marker_helpers(void) {
    char name[MAX_IDENTIFIER_LEN];
    int pending;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, cup_assets_uninstall_is_pending(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_uninstall_is_pending(&pending));
    TEST_ASSERT_FALSE(pending);
    marker_kind = SYSTEM_PATH_REGULAR_FILE;
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_uninstall_is_pending(&pending));
    TEST_ASSERT_TRUE(pending);

    layout_failure = 7;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_assets_uninstall_is_pending(&pending));
    reset_scenario();
    kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, cup_assets_uninstall_is_pending(&pending));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_binary_asset_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("cup-linux-x64", name);
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_platform_checksums_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("SHA256SUMS.linux-x64", name);

    strcpy(host_value, "windows-x64");
    TEST_ASSERT_EQUAL_INT(CUP_OK, cup_assets_binary_asset_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("cup-windows-x64.exe", name);

    host_result = CUP_ERR_INVALID_OS;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_OS, cup_assets_binary_asset_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_OS,
                          cup_assets_platform_checksums_name(name, sizeof(name)));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_assets_binary_asset_name(name, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, cup_assets_platform_checksums_name(name, 2));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_and_complete);
    RUN_TEST(test_bad_assets);
    RUN_TEST(test_inspection_errors);
    RUN_TEST(test_uninstall_fallback);
    RUN_TEST(test_marker_helpers);
    return UNITY_END();
}
