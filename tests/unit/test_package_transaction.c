/*
 * Exercises journal validation and deterministic recovery decisions without
 * duplicating the end-to-end recovery suite.
 */

#include "constants.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "path.h"
#include "runtime_journal.h"
#include "state.h"
#include "system.h"
#include "text.h"
#include "package_transaction.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char root[MAX_PATH_LEN];
static CupError move_result;
static SystemCommitState move_state;
static CupError sync_parent_result;
static CupError ensure_parent_result;
static CupError backup_result;
static CupError remove_tree_result;
static CupError path_kind_result;
static CupError package_validation_result;
static int clear_calls;
static int backup_calls;
static int remove_tree_calls;

/* Fixture lifecycle and local construction helpers. */

static CupError clear_runtime_journal(void) {
    char journal[MAX_PATH_LEN];
    SystemPathIdentity identity;
    CupError err;

    err = layout_get_transaction_path(journal, sizeof(journal));
    if (err != CUP_OK || !test_access_exists(journal)) {
        return err;
    }
    memset(&identity, 0, sizeof(identity));
    err = system_get_path_identity(journal, &identity);
    return err == CUP_OK ? runtime_journal_clear_if_identity(&identity) : err;
}

static CupError build_staging_path_for_test(const PackageTransaction *transaction,
                                            char *buffer,
                                            size_t size) {
    char staging_dir[MAX_PATH_LEN];
    CupError err;

    if (transaction == NULL || buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_staging_dir(staging_dir, sizeof(staging_dir));
    return err == CUP_OK ? path_join(buffer, size, staging_dir, transaction->temporary_name) : err;
}

static CupError begin_package_transaction_for_test(PackageOperation operation,
                                                   const PackageIdentity *package,
                                                   const char *temporary_path) {
    PackageTransaction created;

    return package_transaction_begin(operation, package, temporary_path, &created);
}

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree_real(const char *path) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(path));
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(test_mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void make_valid_package(const char *path) {
    char marker[MAX_PATH_LEN];

    make_dir(path);
    join_test_path(marker, sizeof(marker), path, "valid");
    write_file(marker, "ok");
}

static void set_journal_identity(PackageTransaction *transaction) {
    TEST_ASSERT_NOT_NULL(transaction);
    transaction->file_identity.volume = 0;
    transaction->file_identity.object = 0;
    transaction->file_identity.kind = SYSTEM_PATH_REGULAR_FILE;
    transaction->file_identity.valid = 1;
}

static PackageIdentity package_identity(void) {
    PackageIdentity package;

    memset(&package, 0, sizeof(package));
    strcpy(package.component, "compiler");
    strcpy(package.tool, "clang");
    strcpy(package.host_platform, "linux-x64");
    strcpy(package.target_platform, "linux-x64");
    strcpy(package.version, "22.1.5");
    return package;
}

static void reset_scenario(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];
    char tmp[MAX_PATH_LEN];
    char bin[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        template_path, sizeof(template_path), "cup-transaction-unit"));
    strcpy(root, template_path);
    join_test_path(tmp, sizeof(tmp), root, "staging");
    join_test_path(bin, sizeof(bin), root, "bin");
    make_dir(tmp);
    make_dir(bin);

    move_result = CUP_OK;
    move_state = SYSTEM_COMMIT_DURABLE;
    sync_parent_result = CUP_OK;
    ensure_parent_result = CUP_OK;
    backup_result = CUP_OK;
    remove_tree_result = CUP_OK;
    path_kind_result = CUP_OK;
    package_validation_result = CUP_OK;
    clear_calls = 0;
    backup_calls = 0;
    remove_tree_calls = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
    remove_tree_real(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "transaction.txt");
}

CupError layout_get_root(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "%s", root), size);
}

CupError layout_build_transaction_path(char *buffer, size_t size, const char *selected_root) {
    return path_join(buffer, size, selected_root, "transaction.txt");
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return path_join(buffer, size, root, "staging");
}

CupError layout_build_staging_prefix(char *buffer,
                                     size_t size,
                                     const char *operation,
                                     const PackageIdentity *package) {
    (void)package;
    return buffer_write_result(snprintf(buffer, size, "%s-pkg", operation), size);
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *package) {
    (void)package;
    return path_join(buffer, size, root, "install");
}

CupError layout_ensure_package_parent(const PackageIdentity *package) {
    (void)package;
    return ensure_parent_result;
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    char relative[MAX_PATH_LEN];

    if (text_format(relative, sizeof(relative), "bin/%s", CUP_BINARY_FILENAME) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return path_join(buffer, size, root, relative);
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "checksums.txt");
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "packages.cfg");
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "install.cfg");
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "SHA256SUMS.common");
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    int descriptor;
    int written;

    written = snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (written < 0 || (size_t)written >= path_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    (void)descriptor;
    return test_create_temp_file(directory, prefix, path, path_size, file) == 0
               ? CUP_OK
               : CUP_ERR_TEMPORARY;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_move_path(const char *source, const char *destination, SystemCommitState *state) {
    *state = move_state;
    if (move_result != CUP_OK) {
        if (move_state == SYSTEM_COMMIT_APPLIED && !test_access_exists(destination)) {
            (void)rename(source, destination);
        }
        return move_result;
    }
    if (test_access_exists(destination)) {
        *state = SYSTEM_COMMIT_NOT_APPLIED;
        return CUP_ERR_FILESYSTEM;
    }
    return rename(source, destination) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file_if_identity(const char *source,
                                         const char *destination,
                                         const SystemPathIdentity *expected_identity,
                                         SystemCommitState *state) {
    TEST_ASSERT_NOT_NULL(expected_identity);
    TEST_ASSERT_TRUE(expected_identity->valid);
    return system_move_path(source, destination, state);
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    TestPlatformStat status;

    if (path == NULL || identity == NULL || test_stat_path(path, &status) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    memset(identity, 0, sizeof(*identity));
    identity->kind = test_stat_is_regular(&status) ? SYSTEM_PATH_REGULAR_FILE
                                                   : SYSTEM_PATH_OTHER;
    identity->valid = 1;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}


CupError system_remove_file_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity) {
    (void)expected_identity;
    return system_remove_file(path);
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    clear_calls++;
    return sync_parent_result;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TestPlatformStat status;

    if (path_kind_result != CUP_OK) {
        return path_kind_result;
    }
    if (test_stat_path(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (test_stat_is_regular(&status)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (test_stat_is_directory(&status)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError filesystem_remove_tree(const char *path) {
    remove_tree_calls++;
    if (remove_tree_result != CUP_OK) {
        return remove_tree_result;
    }
    remove_tree_real(path);
    return CUP_OK;
}

CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size) {
    backup_calls++;
    if (backup_result != CUP_OK) {
        return backup_result;
    }
    if (snprintf(backup_path, backup_size, "%s.invalid", path) < 0) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return rename(path, backup_path) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    if (strcmp(version, "bad") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    strcpy(identity->tool, tool);
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, version);
    return CUP_OK;
}

CupError package_validate(const char *base_path,
                          const PackageIdentity *identity,
                          FILE *diagnostics) {
    (void)diagnostics;
    char marker[MAX_PATH_LEN];
    (void)identity;

    if (package_validation_result != CUP_OK) {
        return package_validation_result;
    }
    if (path_join(marker, sizeof(marker), base_path, "valid") != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }
    return test_access_exists(marker) ? CUP_OK : CUP_ERR_VALIDATION;
}

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;

    if (identity == NULL || identity->component[0] == '\0' || identity->tool[0] == '\0' ||
        identity->host_platform[0] == '\0' || identity->target_platform[0] == '\0' ||
        identity->version[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    size_t i;

    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *item = &state->installed[i];
        if (strcmp(item->component, identity->component) == 0 &&
            strcmp(item->tool, identity->tool) == 0 &&
            strcmp(item->host_platform, identity->host_platform) == 0 &&
            strcmp(item->target_platform, identity->target_platform) == 0 &&
            strcmp(item->version, identity->version) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void write_journal_bytes(const void *content, size_t size) {
    char path[MAX_PATH_LEN];
    FILE *file;

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(content, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_journal(const char *content) {
    write_journal_bytes(content, strlen(content));
}

static void set_installed(CupState *state) {
    PackageIdentity *entry = &state->installed[state->installed_count++];

    memset(entry, 0, sizeof(*entry));
    strcpy(entry->component, "compiler");
    strcpy(entry->tool, "clang");
    strcpy(entry->host_platform, "linux-x64");
    strcpy(entry->target_platform, "linux-x64");
    strcpy(entry->version, "22.1.5");
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_init_and_names(void) {
    PackageTransaction transaction;

    memset(&transaction, 0xff, sizeof(transaction));
    package_transaction_init(&transaction);
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_NONE, transaction.operation);
    package_transaction_init(NULL);
    TEST_ASSERT_EQUAL_STRING("install", package_operation_name(PACKAGE_OPERATION_INSTALL));
    TEST_ASSERT_EQUAL_STRING("remove", package_operation_name(PACKAGE_OPERATION_REMOVE));
    TEST_ASSERT_EQUAL_STRING("update", package_operation_name(PACKAGE_OPERATION_UPDATE));
    TEST_ASSERT_EQUAL_STRING("none", package_operation_name(PACKAGE_OPERATION_NONE));
}

static void test_begin_valid(void) {
    PackageIdentity package = package_identity();
    PackageTransaction transaction;
    PackageTransactionStatus status;
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(staging, sizeof(staging), root, "staging/install-pkg-123"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          begin_package_transaction_for_test(
                              PACKAGE_OPERATION_INSTALL, &package, staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_TRANSACTION_LOADED, status);
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_INSTALL, transaction.operation);
    TEST_ASSERT_EQUAL_STRING("install-pkg-123", transaction.temporary_name);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        begin_package_transaction_for_test(PACKAGE_OPERATION_REMOVE, &package, staging));

    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(staging, sizeof(staging), root, "staging/remove-pkg-123"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        begin_package_transaction_for_test(PACKAGE_OPERATION_REMOVE, &package, staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_REMOVE, transaction.operation);

    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());

    write_journal("format=1\n"
                  "operation=update\n"
                  "component=compiler\n"
                  "tool=clang\n"
                  "host_platform=linux-x64\n"
                  "target_platform=linux-x64\n"
                  "package_version=22.1.5\n"
                  "temporary_name=update-pkg-42\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_UPDATE, transaction.operation);
    TEST_ASSERT_EQUAL_STRING("clang", transaction.package.tool);
}

static void test_begin_rejects(void) {
    PackageIdentity package = package_identity();
    PackageTransaction created;
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        begin_package_transaction_for_test(PACKAGE_OPERATION_NONE, &package, "/tmp/x"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        begin_package_transaction_for_test(PACKAGE_OPERATION_INSTALL, NULL, "/tmp/x"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        begin_package_transaction_for_test(PACKAGE_OPERATION_INSTALL, &package, ""));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        begin_package_transaction_for_test(PACKAGE_OPERATION_INSTALL, &package, "/tmp/../x"));

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(staging, sizeof(staging), root, "staging/wrong-123"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          begin_package_transaction_for_test(
                              PACKAGE_OPERATION_INSTALL, &package, staging));

    strcpy(package.tool, "");
    memset(&created, 0xff, sizeof(created));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL,
                                                        &package,
                                                        staging,
                                                        &created));
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_NONE, created.operation);
}

static void test_begin_commit_states(void) {
    PackageIdentity package = package_identity();
    char staging[MAX_PATH_LEN];

    path_join(staging, sizeof(staging), root, "staging/install-pkg-1");
    move_result = CUP_ERR_FILESYSTEM;
    move_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          begin_package_transaction_for_test(
                              PACKAGE_OPERATION_INSTALL, &package, staging));

    move_state = SYSTEM_COMMIT_APPLIED;
    {
        PackageTransaction created;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                              package_transaction_begin(PACKAGE_OPERATION_INSTALL,
                                                        &package,
                                                        staging,
                                                        &created));
        TEST_ASSERT_TRUE(created.file_identity.valid);
        TEST_ASSERT_EQUAL_INT(SYSTEM_PATH_REGULAR_FILE, created.file_identity.kind);
    }
}

static void test_load_status(void) {
    PackageTransaction transaction;
    PackageTransactionStatus status;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_transaction_load(NULL, &status));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_transaction_load(&transaction, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_TRANSACTION_MISSING, status);

    write_journal("journal_version=3\n"
                  "operation=install\n"
                  "component=compiler\n"
                  "tool=clang\n"
                  "host_platform=linux-x64\n"
                  "target_platform=linux-x64\n"
                  "package_version=22.1.5\n"
                  "temporary_name=install-pkg-42\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));
}

static void test_load_invalid(void) {
    PackageTransaction transaction;
    PackageTransactionStatus status;

    write_journal("format=9\noperation=install\n"
                  "component=compiler\ntool=clang\nhost_platform=linux-x64\n"
                  "target_platform=linux-x64\npackage_version=22.1.5\n"
                  "temporary_name=install-pkg-1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    write_journal("format=1\noperation=unknown\n"
                  "temporary_name=unknown-pkg-1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    write_journal("format=1\noperation=install\n"
                  "operation=install\ntemporary_name=install-pkg-1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    write_journal("format=1\noperation=install\n"
                  "component=compiler\ntool=clang\nhost_platform=linux-x64\n"
                  "target_platform=linux-x64\npackage_version=bad\n"
                  "temporary_name=install-pkg-1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    write_journal("format=1\noperation=update\n"
                  "component=compiler\ntool=clang\nhost_platform=linux-x64\n"
                  "target_platform=linux-x64\npackage_version=22.1.5\n"
                  "temporary_name=bad\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    {
        static const unsigned char hidden_nul[] =
            "format=1\noperation=install\ncomponent=compiler\ntool=clang\n"
            "host_platform=linux-x64\ntarget_platform=linux-x64\n"
            "package_version=22.1.5\ntemporary_name=install-pkg-1\0\n";
        write_journal_bytes(hidden_nul, sizeof(hidden_nul) - 1);
        TEST_ASSERT_EQUAL_INT(
            CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));
    }

    write_journal("format=1\noperation=install\ncomponent=compiler\ntool=clang\n"
                  "host_platform=linux-x64\ntarget_platform=linux-x64\n"
                  "package_version=22.1.5\ntemporary_name=install-pkg-1");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));

    write_journal("format=1\r\noperation=install\ncomponent=compiler\ntool=clang\n"
                  "host_platform=linux-x64\ntarget_platform=linux-x64\n"
                  "package_version=22.1.5\ntemporary_name=install-pkg-1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_load(&transaction, &status));
}

static void test_tmp_and_clear(void) {
    PackageTransaction transaction;
    char path[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-4");
    TEST_ASSERT_EQUAL_INT(CUP_OK, build_staging_path_for_test(&transaction, path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "staging/install-pkg-4") != NULL);

    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");
    TEST_ASSERT_EQUAL_INT(CUP_OK, clear_runtime_journal());
    TEST_ASSERT_EQUAL_INT(1, clear_calls);

    write_file(journal, "journal");
    sync_parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, clear_runtime_journal());
    TEST_ASSERT_FALSE(test_access_exists(journal));
}

static void test_recover_installed(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, build_staging_path_for_test(&transaction, staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");

    make_valid_package(staging);
    make_dir(install);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(backup, sizeof(backup), root, "install.invalid"));
    TEST_ASSERT_TRUE(test_access_exists(backup));
    TEST_ASSERT_TRUE(!test_access_exists(staging));
    TEST_ASSERT_TRUE(test_access_exists(install));
}

static void test_recover_existing(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_REMOVE;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "remove-pkg-1");
    layout_build_install_path(install, sizeof(install), &transaction.package);
    build_staging_path_for_test(&transaction, staging, sizeof(staging));
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    make_valid_package(install);
    make_valid_package(staging);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(install));
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_recover_idempotent_existing(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");
    make_valid_package(install);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(install));
}

static void test_recover_absent(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_REMOVE;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "remove-pkg-1");
    layout_build_install_path(install, sizeof(install), &transaction.package);
    build_staging_path_for_test(&transaction, staging, sizeof(staging));
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    make_dir(install);
    make_dir(staging);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(2, remove_tree_calls);
    TEST_ASSERT_TRUE(!test_access_exists(install));
    TEST_ASSERT_TRUE(!test_access_exists(staging));
}

static void test_recover_failures(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(NULL, &state));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(&transaction, &state));
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(&transaction, NULL));
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(&transaction, &state));

    set_journal_identity(&transaction);
    build_staging_path_for_test(&transaction, staging, sizeof(staging));
    make_valid_package(staging);
    path_kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(&transaction, &state));
    path_kind_result = CUP_OK;
    move_result = CUP_ERR_FILESYSTEM;
    move_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_transaction_recover(&transaction, &state));
}

static void test_recover_boundary_failures(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, build_staging_path_for_test(&transaction, staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));

    write_file(journal, "journal");
    make_dir(install);
    make_valid_package(staging);
    backup_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_recover(&transaction, &state));

    backup_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          package_transaction_recover(&transaction, &state));

    backup_result = CUP_ERR_ROLLBACK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK,
                          package_transaction_recover(&transaction, &state));

    backup_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(install));
    ensure_parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          package_transaction_recover(&transaction, &state));

    ensure_parent_result = CUP_OK;
    make_valid_package(install);
    remove_tree_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          package_transaction_recover(&transaction, &state));
}

static void test_recover_preserve_then_move_failure_is_commit(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, build_staging_path_for_test(&transaction, staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, text_format(backup, sizeof(backup), "%s.invalid", install));

    write_file(journal, "journal");
    make_dir(install);
    make_valid_package(staging);
    move_result = CUP_ERR_FILESYSTEM;
    move_state = SYSTEM_COMMIT_NOT_APPLIED;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_TRUE(!test_access_exists(install));
    TEST_ASSERT_TRUE(test_access_exists(backup));
    TEST_ASSERT_TRUE(test_access_exists(staging));
    TEST_ASSERT_TRUE(test_access_exists(journal));
}

static void test_recover_rejects_uncertain_inspection(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    set_journal_identity(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, build_staging_path_for_test(&transaction, staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");
    make_dir(install);
    make_valid_package(staging);

    path_kind_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(journal));

    path_kind_result = CUP_OK;
    package_validation_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
    TEST_ASSERT_TRUE(test_access_exists(journal));
}

static void test_recover_requires_bounded_state_and_journal_identity(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];

    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_REMOVE;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "remove-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    make_dir(install);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_recover(&transaction, &state));
    TEST_ASSERT_TRUE(test_access_exists(install));
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);

    set_journal_identity(&transaction);
    state.installed_count = MAX_INSTALLED + 1u;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_recover(&transaction, &state));
    TEST_ASSERT_TRUE(test_access_exists(install));
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_and_names);
    RUN_TEST(test_begin_valid);
    RUN_TEST(test_begin_rejects);
    RUN_TEST(test_begin_commit_states);
    RUN_TEST(test_load_status);
    RUN_TEST(test_load_invalid);
    RUN_TEST(test_tmp_and_clear);
    RUN_TEST(test_recover_installed);
    RUN_TEST(test_recover_existing);
    RUN_TEST(test_recover_idempotent_existing);
    RUN_TEST(test_recover_absent);
    RUN_TEST(test_recover_failures);
    RUN_TEST(test_recover_boundary_failures);
    RUN_TEST(test_recover_preserve_then_move_failure_is_commit);
    RUN_TEST(test_recover_rejects_uncertain_inspection);
    RUN_TEST(test_recover_requires_bounded_state_and_journal_identity);
    return UNITY_END();
}
