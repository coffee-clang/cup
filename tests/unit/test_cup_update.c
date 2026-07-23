/*
 * Test focus: Exercises official-version selection, downgrade/equality policy, staged asset
 * verification and deferred-helper boundaries.
 */

#include "cup_assets.h"
#include "download.h"
#include "checksum.h"
#include "command_context.h"
#include "commands.h"
#include "error.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "system.h"
#include "package_transaction.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char temp_dir[CUP_TEST_TEMP_PATH_SIZE];
static char remote_version[64];
static char remote_commit[64];
static char versioned_version[64];
static char versioned_commit[64];
static int fetch_calls;
static int fail_fetch_call;
static int checksum_schema_valid;
static int checksum_matches;
static CupError verify_result;
static int verify_calls;
static int fail_verify_call;
static CupError executable_result;
static int fail_executable_call;
static int setup_calls;
static int fail_setup_call;
static int latest_metadata_mode;
static int versioned_metadata_mode;
static CupError context_result;
static CupError transaction_check_result;
static CupError transaction_begin_result;
static CupError transaction_clear_result;
static CupError helper_prepare_result;
static CupError helper_result;
static CupError cleanup_result;
static int context_end_calls;
static int transaction_begin_calls;
static int transaction_clear_calls;
static int helper_prepare_calls;
static int helper_calls;
static int cleanup_calls;
static int executable_calls;
static unsigned staging_serial;
static CupError cup_assets_inspect_result;
static int installed_generation_valid;
static int cup_assets_inspect_calls;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    strcpy(remote_version, "1.2.4");
    strcpy(remote_commit, "abcdef1");
    strcpy(versioned_version, "1.2.4");
    strcpy(versioned_commit, "abcdef1");
    fetch_calls = 0;
    fail_fetch_call = 0;
    checksum_schema_valid = 1;
    checksum_matches = 1;
    verify_result = CUP_OK;
    verify_calls = 0;
    fail_verify_call = 0;
    executable_result = CUP_OK;
    fail_executable_call = 0;
    setup_calls = 0;
    fail_setup_call = 0;
    latest_metadata_mode = 0;
    versioned_metadata_mode = 0;
    context_result = CUP_OK;
    transaction_check_result = CUP_OK;
    transaction_begin_result = CUP_OK;
    transaction_clear_result = CUP_OK;
    helper_prepare_result = CUP_OK;
    helper_result = CUP_OK;
    cleanup_result = CUP_OK;
    context_end_calls = 0;
    transaction_begin_calls = 0;
    transaction_clear_calls = 0;
    helper_prepare_calls = 0;
    helper_calls = 0;
    cleanup_calls = 0;
    executable_calls = 0;
    cup_assets_inspect_result = CUP_OK;
    installed_generation_valid = 1;
    cup_assets_inspect_calls = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_metadata(const char *path, const char *version, const char *commit, int mode) {
    char contents[512];

    switch (mode) {
        case 1:
            (void)snprintf(
                contents, sizeof(contents), "format=2\nversion=%s\ncommit=%s\n", version, commit);
            break;
        case 2:
            (void)snprintf(contents,
                           sizeof(contents),
                           "format=1\nversion=%s\nversion=%s\ncommit=%s\n",
                           version,
                           version,
                           commit);
            break;
        case 3:
            (void)snprintf(contents,
                           sizeof(contents),
                           "format=1\nunknown=x\nversion=%s\ncommit=%s\n",
                           version,
                           commit);
            break;
        case 4:
            (void)snprintf(contents, sizeof(contents), "format=1\nversion=%s\n", version);
            break;
        case 5:
            (void)snprintf(contents, sizeof(contents), "not-key-value\n");
            break;
        default:
            (void)snprintf(
                contents, sizeof(contents), "format=1\nversion=%s\ncommit=%s\n", version, commit);
            break;
    }
    write_text(path, contents);
}

static CupError setup_result(void) {
    setup_calls++;
    return setup_calls == fail_setup_call ? CUP_ERR_BUFFER_TOO_SMALL : CUP_OK;
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)target_override;
    (void)mode;
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
    }
    return context_result;
}

void command_context_end(CommandContext *context) {
    (void)context;
    context_end_calls++;
}

CupError runtime_journal_require_none(void) {
    return transaction_check_result;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    cup_assets_inspect_calls++;
    if (inspection == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(inspection, 0, sizeof(*inspection));
    if (cup_assets_inspect_result != CUP_OK) {
        return cup_assets_inspect_result;
    }
    if (installed_generation_valid) {
        inspection->binary = CUP_ASSET_VALID;
        inspection->helper = CUP_ASSET_VALID;
        inspection->catalog = CUP_ASSET_VALID;
        inspection->install_policy = CUP_ASSET_VALID;
        inspection->uninstall = CUP_ASSET_VALID;
        inspection->common_checksums = CUP_ASSET_VALID;
        inspection->platform_checksums = CUP_ASSET_VALID;
    }
    return CUP_OK;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    TEST_ASSERT_NOT_NULL(inspection);
    return installed_generation_valid;
}

CupError cup_assets_binary_asset_name(char *name, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return buffer_write_result(snprintf(name, size, "cup-linux-x64"), size);
}

CupError cup_assets_platform_checksums_name(char *name, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return buffer_write_result(snprintf(name, size, "SHA256SUMS.linux-x64"), size);
}

CupError cup_assets_verify_asset(const char *checksum_path,
                                 const char *asset_name,
                                 const char *asset_path,
                                 int *matches) {
    (void)checksum_path;
    (void)asset_name;
    (void)asset_path;
    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    verify_calls++;
    if (verify_calls == fail_verify_call) {
        return verify_result;
    }
    *matches = checksum_matches;
    return CUP_OK;
}

static CupError copy_test_path(char *buffer, size_t size, const char *suffix) {
    return buffer_write_result(snprintf(buffer, size, "%s/%s", temp_dir, suffix), size);
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "tmp");
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/cup");
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/uninstall.sh");
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/SHA256SUMS.linux-x64");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/packages.cfg");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/install.cfg");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "installed/SHA256SUMS.common");
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "cup.lock");
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    if (setup_result() != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return copy_test_path(buffer, size, "tmp/transaction.txt");
}

CupError system_create_temp_directory(const char *directory,
                                      const char *prefix,
                                      char *path,
                                      size_t path_size) {
    (void)directory;
    TEST_ASSERT_EQUAL_STRING("cup-update", prefix);
    if (setup_result() != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }
    staging_serial++;
    if (snprintf(path, path_size, "%s/staging-%u", temp_dir, staging_serial) <= 0) {
        return CUP_ERR_TEMPORARY;
    }
    return test_mkdir(path, 0700) == 0 ? CUP_OK : CUP_ERR_TEMPORARY;
}

CupError system_set_executable(const char *path, int executable) {
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_TRUE(executable);
    executable_calls++;
    if (executable_calls == fail_executable_call) {
        return executable_result;
    }
    return CUP_OK;
}

unsigned long system_get_process_id(void) {
    return 1234;
}

CupError cup_update_helper_prepare(void) {
    helper_prepare_calls++;
    return helper_prepare_result;
}

CupError cup_update_helper_start(const char *token) {
    TEST_ASSERT_NOT_NULL(token);
    TEST_ASSERT_NOT_NULL(strstr(token, "u1234-staging-"));
    helper_calls++;
    return helper_result;
}

CupError download_file(const char *url, const char *destination, DownloadValidation validation) {
    (void)validation;
    fetch_calls++;
    if (fail_fetch_call == fetch_calls) {
        return CUP_ERR_FETCH;
    }

    if (fetch_calls == 1) {
        TEST_ASSERT_TRUE(strstr(url, "/latest/download/release.txt") != NULL);
        write_metadata(destination, remote_version, remote_commit, latest_metadata_mode);
    } else if (fetch_calls == 2) {
        TEST_ASSERT_TRUE(strstr(url, "SHA256SUMS.linux-x64") != NULL);
        write_text(destination, "platform checksums\n");
    } else if (fetch_calls == 3) {
        TEST_ASSERT_TRUE(strstr(url, "SHA256SUMS.common") != NULL);
        write_text(destination, "common checksums\n");
    } else if (fetch_calls == 4) {
        TEST_ASSERT_TRUE(strstr(url, "/download/v") != NULL);
        write_metadata(destination, versioned_version, versioned_commit, versioned_metadata_mode);
    } else if (fetch_calls == 5) {
        TEST_ASSERT_TRUE(strstr(url, "cup-linux-x64") != NULL);
        write_text(destination, "binary\n");
    } else if (fetch_calls == 6) {
        TEST_ASSERT_TRUE(strstr(url, "uninstall.sh") != NULL);
        write_text(destination, "#!/bin/sh\n");
    } else if (fetch_calls == 7) {
        TEST_ASSERT_TRUE(strstr(url, "packages.cfg") != NULL);
        write_text(destination, "catalog\n");
    } else if (fetch_calls == 8) {
        TEST_ASSERT_TRUE(strstr(url, "install.cfg") != NULL);
        write_text(destination, "install config\n");
    } else {
        TEST_FAIL_MESSAGE("unexpected cup update fetch");
        return CUP_ERR_FETCH;
    }
    return CUP_OK;
}

CupError checksum_validate_assets(const char *checksum_path,
                                  const char *const *asset_names,
                                  size_t asset_count) {
    TEST_ASSERT_NOT_NULL(checksum_path);
    TEST_ASSERT_NOT_NULL(asset_names);
    TEST_ASSERT_TRUE(asset_count == 3 || asset_count == 2);
    return checksum_schema_valid ? CUP_OK : CUP_ERR_VALIDATION;
}

void package_catalog_init(PackageCatalog *catalog) {
    if (catalog != NULL) {
        memset(catalog, 0, sizeof(*catalog));
    }
}

void package_catalog_free(PackageCatalog *catalog) {
    (void)catalog;
}

CupError package_catalog_load_path(PackageCatalog *catalog,
                                   const char *path,
                                   PackageCatalogSource source) {
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_INT(PACKAGE_CATALOG_SOURCE_INSTALLED, source);
    return CUP_OK;
}

void install_policy_init(InstallPolicy *config) {
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
    }
}

CupError install_policy_load_path(InstallPolicy *config,
                                  const char *path,
                                  InstallPolicySource source) {
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_INT(INSTALL_POLICY_SOURCE_INSTALLED, source);
    return CUP_OK;
}

CupError cup_update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version) {
    TEST_ASSERT_NOT_NULL(temporary_path);
    TEST_ASSERT_NOT_NULL(token);
    TEST_ASSERT_NOT_NULL(strstr(token, "u1234-staging-"));
    TEST_ASSERT_EQUAL_STRING(versioned_version, version);
    transaction_begin_calls++;
    return transaction_begin_result;
}

CupError cup_update_journal_clear(void) {
    transaction_clear_calls++;
    return transaction_clear_result;
}

CupError filesystem_remove_tree(const char *path) {
    TEST_ASSERT_NOT_NULL(path);
    cleanup_calls++;
    return cleanup_result;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_installed_preflight(void) {
    installed_generation_valid = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, cup_assets_inspect_calls);
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);
    TEST_ASSERT_EQUAL_INT(0, setup_calls);

    reset_scenario();
    cup_assets_inspect_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, cup_assets_inspect_calls);
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);
}

static void test_update_success(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update_cup());
    TEST_ASSERT_EQUAL_INT(8, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_prepare_calls);
    TEST_ASSERT_EQUAL_INT(1, transaction_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, helper_calls);
    TEST_ASSERT_EQUAL_INT(2, executable_calls);
    TEST_ASSERT_EQUAL_INT(0, transaction_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
}

static void test_noop_versions(void) {
    strcpy(remote_version, "1.2.3");
    strcpy(versioned_version, "1.2.3");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, fetch_calls);
    TEST_ASSERT_EQUAL_INT(0, transaction_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, helper_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    strcpy(remote_version, "1.2.2");
    strcpy(versioned_version, "1.2.2");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
}

static void test_bad_latest_metadata(void) {
    strcpy(remote_version, "01.2.4");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    strcpy(remote_commit, "INVALID");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
}

static void test_versioned_checksums(void) {
    strcpy(versioned_commit, "abcdef2");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(4, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    checksum_schema_valid = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(4, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    checksum_matches = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(4, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
}

static void test_update_fetch_fail(void) {
    context_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_update_cup());
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);

    reset_scenario();
    transaction_check_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);

    reset_scenario();
    fail_fetch_call = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    fail_fetch_call = 5;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
}

static void test_update_commit_fail(void) {
    helper_prepare_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());
    TEST_ASSERT_EQUAL_INT(0, transaction_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    transaction_begin_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(0, transaction_clear_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    transaction_begin_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    helper_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    reset_scenario();
    helper_result = CUP_ERR_FILESYSTEM;
    transaction_clear_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_update_cup());
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
}

static void test_version_order(void) {
    strcpy(remote_version, "2.0.0");
    strcpy(versioned_version, "2.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update_cup());

    reset_scenario();
    strcpy(remote_version, "1.3.0");
    strcpy(versioned_version, "1.3.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update_cup());
}

static void test_metadata_shapes(void) {
    const char *versions[] = {"", "1.2", "1.2.3.4", "1000000.2.3"};
    size_t i;

    for (i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
        reset_scenario();
        strcpy(remote_version, versions[i]);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    }

    for (i = 1; i <= 5; ++i) {
        reset_scenario();
        latest_metadata_mode = (int)i;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
    }

    reset_scenario();
    strcpy(remote_commit, "abcdef");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());

    reset_scenario();
    strcpy(remote_commit, "ABCDEF1");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());

    reset_scenario();
    versioned_metadata_mode = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_update_cup());
}

static void test_setup_failures(void) {
    int step;

    for (step = 1; step <= 12; ++step) {
        reset_scenario();
        fail_setup_call = step;
        TEST_ASSERT_NOT_EQUAL(CUP_OK, command_update_cup());
        TEST_ASSERT_EQUAL_INT(0, fetch_calls);
    }
}

static void test_stage_failures(void) {
    int call;

    for (call = 2; call <= 8; ++call) {
        reset_scenario();
        fail_fetch_call = call;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update_cup());
        TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    }

    reset_scenario();
    fail_verify_call = 1;
    verify_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());

    reset_scenario();
    fail_verify_call = 2;
    verify_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());

    reset_scenario();
    fail_executable_call = 1;
    executable_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());

    reset_scenario();
    fail_executable_call = 2;
    executable_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_update_cup());
}

/* Suite registration. */

int main(void) {
    char tmp_path[1024];

    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        temp_dir, sizeof(temp_dir), "cup-cup-update-test"));
    TEST_ASSERT_TRUE(snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(tmp_path, 0700));
    TEST_ASSERT_TRUE(snprintf(tmp_path, sizeof(tmp_path), "%s/installed", temp_dir) > 0);
    TEST_ASSERT_EQUAL_INT(0, test_mkdir(tmp_path, 0700));

    UNITY_BEGIN();
    RUN_TEST(test_installed_preflight);
    RUN_TEST(test_update_success);
    RUN_TEST(test_noop_versions);
    RUN_TEST(test_version_order);
    RUN_TEST(test_bad_latest_metadata);
    RUN_TEST(test_metadata_shapes);
    RUN_TEST(test_setup_failures);
    RUN_TEST(test_stage_failures);
    RUN_TEST(test_versioned_checksums);
    RUN_TEST(test_update_fetch_fail);
    RUN_TEST(test_update_commit_fail);
    return UNITY_END();
}
