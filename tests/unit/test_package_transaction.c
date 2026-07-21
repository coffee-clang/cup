/*
 * Test focus: Exercises journal validation and deterministic recovery decisions without
 * duplicating the end-to-end recovery suite.
 */

#include "cup_assets.h"
#include "package_selector.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "path.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "unity.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[MAX_PATH_LEN];
static CupError path_exists_result;
static CupError replace_result;
static SystemCommitState replace_state;
static CupError move_result;
static SystemCommitState move_state;
static CupError sync_parent_result;
static CupError remove_result;
static CupError permission_result;
static int cup_assets_valid;
static int legacy_cup_assets_valid;
static int clear_calls;
static int backup_calls;
static int remove_tree_calls;

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    TEST_ASSERT_TRUE(snprintf(buffer, size, "%s/%s", left, right) > 0);
}

static void remove_tree_real(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        unlink(path);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[MAX_PATH_LEN];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        join_test_path(child, sizeof(child), path, entry->d_name);
        remove_tree_real(child);
    }
    closedir(directory);
    rmdir(path);
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(mkdir(path, 0700) == 0 || errno == EEXIST);
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
    char template_path[] = "/tmp/cup-transaction-unit-XXXXXX";
    char tmp[MAX_PATH_LEN];
    char bin[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(mkdtemp(template_path));
    strcpy(root, template_path);
    join_test_path(tmp, sizeof(tmp), root, "staging");
    join_test_path(bin, sizeof(bin), root, "bin");
    make_dir(tmp);
    make_dir(bin);

    path_exists_result = CUP_OK;
    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    move_result = CUP_OK;
    move_state = SYSTEM_COMMIT_DURABLE;
    sync_parent_result = CUP_OK;
    remove_result = CUP_OK;
    permission_result = CUP_OK;
    cup_assets_valid = 1;
    legacy_cup_assets_valid = 0;
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

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "transaction.txt");
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return path_join(buffer, size, root, "staging");
}

CupError layout_build_staging_prefix(char *buffer,
                                     size_t size,
                                     const char *operation,
                                     const PackageIdentity *package) {
    (void)package;
    return snprintf(buffer, size, "%s-pkg", operation) > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *package) {
    (void)package;
    return path_join(buffer, size, root, "install");
}

CupError layout_ensure_package_parent(const PackageIdentity *package) {
    (void)package;
    return CUP_OK;
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/cup");
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return path_join(buffer, size, root, "bin/uninstall.sh");
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

    if (snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix) < 0) {
        return CUP_ERR_TEMPORARY;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return CUP_ERR_TEMPORARY;
    }
    *file = fdopen(descriptor, "w+b");
    return *file == NULL ? CUP_ERR_TEMPORARY : CUP_OK;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = replace_state;
    if (replace_result != CUP_OK) {
        return replace_result;
    }
    return rename(source, destination) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_move_path(const char *source, const char *destination, SystemCommitState *state) {
    *state = move_state;
    if (move_result != CUP_OK) {
        return move_result;
    }
    return rename(source, destination) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_path_exists(const char *path, int *exists) {
    if (path_exists_result != CUP_OK) {
        return path_exists_result;
    }
    *exists = access(path, F_OK) == 0;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    if (remove_result != CUP_OK) {
        return remove_result;
    }
    return unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    (void)path;
    clear_calls++;
    return sync_parent_result;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    struct stat status;

    if (lstat(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (S_ISREG(status.st_mode)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (S_ISDIR(status.st_mode)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else if (S_ISLNK(status.st_mode)) {
        *kind = SYSTEM_PATH_LINK;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    (void)path;
    (void)executable;
    return permission_result;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    (void)read_only;
    return permission_result;
}

CupError filesystem_remove_tree(const char *path) {
    remove_tree_calls++;
    remove_tree_real(path);
    return CUP_OK;
}

CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size) {
    backup_calls++;
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

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    char marker[MAX_PATH_LEN];
    (void)identity;

    if (path_join(marker, sizeof(marker), base_path, "valid") != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }
    return access(marker, F_OK) == 0 ? CUP_OK : CUP_ERR_VALIDATION;
}

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release) {
    return snprintf(buffer, size, "%s@%s", tool, release) > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
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

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    memset(inspection, 0, sizeof(*inspection));
    return CUP_OK;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    (void)inspection;
    return cup_assets_valid;
}

static void write_journal(const char *content) {
    char path[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(path, sizeof(path)));
    write_file(path, content);
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
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_TRANSACTION_LOADED, status);
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_INSTALL, transaction.operation);
    TEST_ASSERT_EQUAL_STRING("install-pkg-123", transaction.temporary_name);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_begin(PACKAGE_OPERATION_REMOVE, &package, staging));

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_clear());
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          path_join(staging, sizeof(staging), root, "staging/remove-pkg-123"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_transaction_begin(PACKAGE_OPERATION_REMOVE, &package, staging));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_load(&transaction, &status));
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_REMOVE, transaction.operation);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_clear());

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
    char staging[MAX_PATH_LEN];

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_begin(PACKAGE_OPERATION_NONE, &package, "/tmp/x"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, NULL, "/tmp/x"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, ""));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_TRANSACTION,
        package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, "/tmp/../x"));

    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(staging, sizeof(staging), root, "staging/wrong-123"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, staging));
}

static void test_begin_commit_states(void) {
    PackageIdentity package = package_identity();
    char staging[MAX_PATH_LEN];

    path_join(staging, sizeof(staging), root, "staging/install-pkg-1");
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, staging));

    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          package_transaction_begin(PACKAGE_OPERATION_INSTALL, &package, staging));
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
}

static void test_tmp_and_clear(void) {
    PackageTransaction transaction;
    char path[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-4");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_get_staging_path(NULL, path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_transaction_get_staging_path(&transaction, NULL, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_transaction_get_staging_path(&transaction, path, sizeof(path)));
    TEST_ASSERT_TRUE(strstr(path, "staging/install-pkg-4") != NULL);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_clear());
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_clear());
    TEST_ASSERT_EQUAL_INT(1, clear_calls);

    write_file(journal, "journal");
    sync_parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_clear());
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
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, layout_build_install_path(install, sizeof(install), &transaction.package));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_transaction_get_staging_path(&transaction, staging, sizeof(staging)));
    TEST_ASSERT_EQUAL_INT(CUP_OK, layout_get_transaction_path(journal, sizeof(journal)));
    write_file(journal, "journal");

    make_valid_package(staging);
    make_dir(install);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(1, backup_calls);
    TEST_ASSERT_EQUAL_INT(CUP_OK, path_join(backup, sizeof(backup), root, "install.invalid"));
    TEST_ASSERT_TRUE(access(backup, F_OK) == 0);
    TEST_ASSERT_TRUE(access(staging, F_OK) != 0);
    TEST_ASSERT_TRUE(access(install, F_OK) == 0);
}

static void test_recover_existing(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_REMOVE;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "remove-pkg-1");
    layout_build_install_path(install, sizeof(install), &transaction.package);
    package_transaction_get_staging_path(&transaction, staging, sizeof(staging));
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    make_valid_package(install);
    make_valid_package(staging);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_TRUE(access(install, F_OK) == 0);
    TEST_ASSERT_TRUE(access(staging, F_OK) != 0);
}

static void test_recover_absent(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char install[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_REMOVE;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "remove-pkg-1");
    layout_build_install_path(install, sizeof(install), &transaction.package);
    package_transaction_get_staging_path(&transaction, staging, sizeof(staging));
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    make_dir(install);
    make_dir(staging);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_transaction_recover(&transaction, &state));
    TEST_ASSERT_EQUAL_INT(2, remove_tree_calls);
    TEST_ASSERT_TRUE(access(install, F_OK) != 0);
    TEST_ASSERT_TRUE(access(staging, F_OK) != 0);
}

static void test_recover_failures(void) {
    PackageTransaction transaction;
    CupState state = {0};
    char staging[MAX_PATH_LEN];
    char journal[MAX_PATH_LEN];

    set_installed(&state);
    package_transaction_init(&transaction);
    transaction.operation = PACKAGE_OPERATION_INSTALL;
    transaction.package = package_identity();
    strcpy(transaction.temporary_name, "install-pkg-1");
    layout_get_transaction_path(journal, sizeof(journal));
    write_file(journal, "journal");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, package_transaction_recover(&transaction, &state));

    package_transaction_get_staging_path(&transaction, staging, sizeof(staging));
    make_valid_package(staging);
    move_result = CUP_ERR_FILESYSTEM;
    move_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_transaction_recover(&transaction, &state));
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
    RUN_TEST(test_recover_absent);
    RUN_TEST(test_recover_failures);
    return UNITY_END();
}
