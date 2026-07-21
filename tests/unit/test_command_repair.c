/*
 * Test focus: Exercises repair ordering, state reconciliation, quarantine and cleanup decisions
 * without duplicating filesystem recovery.
 */

#include "cup_assets.h"
#include "download.h"
#include "checksum.h"
#include "commands.h"
#include "package_selector.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "cup_update_journal.h"
#include "cup_update_helper.h"
#include "runtime_journal.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#define MAX_STEPS 4

static CupState loaded_state;
static StateFileStatus loaded_status;
static CupError root_result;
static CupError lock_path_result;
static CupError lock_result;
static CupError pending_result;
static int pending_uninstall;
static CupError runtime_result;
static CupError state_load_result;
static CupError state_validate_result;
static CupError transaction_results[MAX_STEPS];
static PackageTransactionStatus transaction_statuses[MAX_STEPS];
static PackageTransaction transactions[MAX_STEPS];
static size_t transaction_calls;
static CupError recover_result;
static CupAssetsInspection cup_assets_inspection;
static CupError cup_assets_result;
static int has_installed_assets;
static int development_valid;
static CupError ensure_cup_assets_result;
static PackageList scan_lists[MAX_STEPS];
static CupError scan_results[MAX_STEPS];
static size_t scan_calls;
static CupError quarantine_result;
static CupError state_save_result;
static CupError plan_build_result;
static CupError plan_apply_result;
static CupError cleanup_result;
static CupError backup_result;
static int metadata_read_only;
static int cup_assets_read_only;
static int cup_assets_executable;
static int verify_matches;
static int cup_assets_files_regular;
static int fetch_calls;
static int lock_release_calls;
static int backup_calls;
static int recover_calls;
static int save_calls;
static int quarantine_calls;
static int plan_build_calls;
static int plan_apply_calls;
static int cleanup_calls;
static int set_metadata_calls;
static int set_read_only_calls;
static int set_executable_calls;
static int destination_exists;
static CupError replace_result;
static SystemCommitState replace_state;
static CupError restore_move_result;
static int install_policy_regular_override;
static CupError install_policy_regular_result;
static int install_policy_regular;
static int install_policy_read_only_override;
static CupError install_policy_read_only_result;
static int install_policy_read_only;
static int install_policy_load_override;
static CupError install_policy_load_results[2];
static size_t install_policy_load_calls;
static int install_policy_verify_override;
static CupError install_policy_verify_results[2];
static int install_policy_verify_matches[2];
static size_t install_policy_verify_calls;
static CupError install_policy_fetch_result;
static int install_policy_replace_override;
static CupError install_policy_replace_result;
static SystemCommitState install_policy_replace_state;

static void reset_scenario(void) {
    size_t i;

    memset(&loaded_state, 0, sizeof(loaded_state));
    loaded_status = STATE_FILE_LOADED;
    root_result = CUP_OK;
    lock_path_result = CUP_OK;
    lock_result = CUP_OK;
    pending_result = CUP_OK;
    pending_uninstall = 0;
    runtime_result = CUP_OK;
    state_load_result = CUP_OK;
    state_validate_result = CUP_OK;
    transaction_calls = 0;
    recover_result = CUP_OK;
    memset(&cup_assets_inspection, 0, sizeof(cup_assets_inspection));
    cup_assets_result = CUP_OK;
    has_installed_assets = 0;
    development_valid = 1;
    ensure_cup_assets_result = CUP_OK;
    scan_calls = 0;
    quarantine_result = CUP_OK;
    state_save_result = CUP_OK;
    plan_build_result = CUP_OK;
    plan_apply_result = CUP_OK;
    cleanup_result = CUP_OK;
    backup_result = CUP_OK;
    metadata_read_only = 1;
    cup_assets_read_only = 1;
    cup_assets_executable = 1;
    verify_matches = 1;
    cup_assets_files_regular = 1;
    fetch_calls = 0;
    lock_release_calls = 0;
    backup_calls = 0;
    recover_calls = 0;
    save_calls = 0;
    quarantine_calls = 0;
    plan_build_calls = 0;
    plan_apply_calls = 0;
    cleanup_calls = 0;
    set_metadata_calls = 0;
    set_read_only_calls = 0;
    set_executable_calls = 0;
    destination_exists = 0;
    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    restore_move_result = CUP_OK;
    install_policy_regular_override = 0;
    install_policy_regular_result = CUP_OK;
    install_policy_regular = 1;
    install_policy_read_only_override = 0;
    install_policy_read_only_result = CUP_OK;
    install_policy_read_only = 1;
    install_policy_load_override = 0;
    install_policy_load_results[0] = CUP_OK;
    install_policy_load_results[1] = CUP_OK;
    install_policy_load_calls = 0;
    install_policy_verify_override = 0;
    install_policy_verify_results[0] = CUP_OK;
    install_policy_verify_results[1] = CUP_OK;
    install_policy_verify_matches[0] = 1;
    install_policy_verify_matches[1] = 1;
    install_policy_verify_calls = 0;
    install_policy_fetch_result = CUP_OK;
    install_policy_replace_override = 0;
    install_policy_replace_result = CUP_OK;
    install_policy_replace_state = SYSTEM_COMMIT_DURABLE;

    for (i = 0; i < MAX_STEPS; ++i) {
        transaction_results[i] = CUP_OK;
        transaction_statuses[i] = PACKAGE_TRANSACTION_MISSING;
        package_transaction_init(&transactions[i]);
        memset(&scan_lists[i], 0, sizeof(scan_lists[i]));
        scan_lists[i].complete = 1;
        scan_results[i] = CUP_OK;
    }
}

CupError platform_get_host(char *buffer, size_t size) {
    return snprintf(buffer, size, "linux-x64") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_update_helper_prepare(void) {
    return CUP_OK;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

static PackageIdentity state_entry(const char *entry) {
    PackageIdentity item;
    const char *separator = strchr(entry, '@');
    size_t tool_length;

    TEST_ASSERT_NOT_NULL(separator);
    tool_length = (size_t)(separator - entry);
    memset(&item, 0, sizeof(item));
    strcpy(item.component, "compiler");
    memcpy(item.tool, entry, tool_length);
    item.tool[tool_length] = '\0';
    strcpy(item.host_platform, "linux-x64");
    strcpy(item.target_platform, "linux-x64");
    strcpy(item.version, separator + 1);
    return item;
}

static PackageIdentity package_identity(const char *version) {
    PackageIdentity package;

    memset(&package, 0, sizeof(package));
    strcpy(package.component, "compiler");
    strcpy(package.tool, "clang");
    strcpy(package.host_platform, "linux-x64");
    strcpy(package.target_platform, "linux-x64");
    strcpy(package.version, version);
    return package;
}

CupError layout_ensure_root(void) {
    return root_result;
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    if (lock_path_result != CUP_OK) {
        return lock_path_result;
    }
    return snprintf(buffer, size, "/tmp/cup.lock") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_EQUAL_STRING("/tmp/cup.lock", path);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    lock->active = lock_result == CUP_OK;
    return lock_result;
}

void system_lock_release(SystemLock *lock) {
    TEST_ASSERT_NOT_NULL(lock);
    lock_release_calls++;
    lock->active = 0;
}

CupError cup_assets_uninstall_is_pending(int *pending) {
    if (pending_result != CUP_OK) {
        return pending_result;
    }
    *pending = pending_uninstall;
    return CUP_OK;
}

CupError layout_ensure_runtime(void) {
    return runtime_result;
}

CupError state_load(CupState *state, StateFileStatus *status) {
    *state = loaded_state;
    *status = loaded_status;
    return state_load_result;
}

CupError state_validate(const CupState *state) {
    (void)state;
    return state_validate_result;
}

CupError runtime_journal_detect(RuntimeJournalKind *kind) {
    size_t index = transaction_calls;

    TEST_ASSERT_TRUE(index < MAX_STEPS);
    if (transaction_results[index] != CUP_OK) {
        return transaction_results[index];
    }
    *kind = transaction_statuses[index] == PACKAGE_TRANSACTION_LOADED ? RUNTIME_JOURNAL_PACKAGE
                                                                      : RUNTIME_JOURNAL_MISSING;
    return CUP_OK;
}

void cup_update_journal_init(CupUpdateJournal *journal) {
    if (journal != NULL) {
        memset(journal, 0, sizeof(*journal));
    }
}

CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status) {
    (void)journal;
    *status = CUP_UPDATE_JOURNAL_MISSING;
    return CUP_OK;
}

CupError cup_update_journal_recover(const CupUpdateJournal *journal) {
    (void)journal;
    return recover_result;
}

void package_transaction_init(PackageTransaction *transaction) {
    if (transaction != NULL) {
        memset(transaction, 0, sizeof(*transaction));
    }
}

CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status) {
    size_t index = transaction_calls++;

    TEST_ASSERT_TRUE(index < MAX_STEPS);
    *transaction = transactions[index];
    *status = transaction_statuses[index];
    return transaction_results[index];
}

CupError package_transaction_recover(const PackageTransaction *transaction, CupState *state) {
    TEST_ASSERT_NOT_NULL(transaction);
    TEST_ASSERT_NOT_NULL(state);
    recover_calls++;
    return recover_result;
}

CupError layout_get_state_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/state.txt") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/transaction.txt") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError filesystem_backup_invalid(const char *path, char *backup_path, size_t backup_size) {
    backup_calls++;
    if (backup_result != CUP_OK) {
        return backup_result;
    }
    return snprintf(backup_path, backup_size, "%s.invalid", path) > 0 ? CUP_OK
                                                                      : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    *inspection = cup_assets_inspection;
    return cup_assets_result;
}

int cup_assets_has_installed_assets(const CupAssetsInspection *inspection) {
    (void)inspection;
    return has_installed_assets;
}

int cup_assets_development_is_valid(const CupAssetsInspection *inspection) {
    (void)inspection;
    return development_valid;
}

CupError layout_ensure_cup_assets(void) {
    return ensure_cup_assets_result;
}

CupError package_scan(PackageList *packages) {
    size_t index = scan_calls++;

    TEST_ASSERT_TRUE(index < MAX_STEPS);
    *packages = scan_lists[index];
    return scan_results[index];
}

int package_list_contains(const PackageList *packages, const PackageIdentity *package) {
    size_t i;

    for (i = 0; i < packages->count; ++i) {
        const PackageIdentity *item = &packages->items[i];
        if (strcmp(item->component, package->component) == 0 &&
            strcmp(item->tool, package->tool) == 0 &&
            strcmp(item->host_platform, package->host_platform) == 0 &&
            strcmp(item->target_platform, package->target_platform) == 0 &&
            strcmp(item->version, package->version) == 0) {
            return 1;
        }
    }
    return 0;
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *entry) {
    const char *separator = strchr(entry, '@');
    size_t tool_length;

    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    tool_length = (size_t)(separator - entry);
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    memcpy(identity->tool, entry, tool_length);
    identity->tool[tool_length] = '\0';
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, separator + 1);
    return CUP_OK;
}

CupError package_identity_validate(const PackageIdentity *identity) {
    return identity != NULL && identity->component[0] != '\0' && identity->tool[0] != '\0' &&
                   identity->host_platform[0] != '\0' && identity->target_platform[0] != '\0' &&
                   identity->version[0] != '\0'
               ? CUP_OK
               : CUP_ERR_INVALID_INPUT;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    int written;
    if (package_identity_validate(identity) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    written = snprintf(buffer, size, "%s@%s", identity->tool, identity->version);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope) {
    if (package_identity_validate(identity) != CUP_OK || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(scope, 0, sizeof(*scope));
    strcpy(scope->component, identity->component);
    strcpy(scope->host_platform, identity->host_platform);
    strcpy(scope->target_platform, identity->target_platform);
    return CUP_OK;
}

static int identities_equal(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    size_t i;

    for (i = 0; i < state->installed_count; ++i) {
        if (identities_equal(&state->installed[i], identity)) {
            return (int)i;
        }
    }
    return -1;
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    state->installed[state->installed_count++] = *identity;
    return CUP_OK;
}

CupError state_remove_installed(CupState *state, const PackageIdentity *identity) {
    int index = state_find_installed(state, identity);

    if (index < 0) {
        return CUP_ERR_NOT_INSTALLED;
    }
    memmove(&state->installed[index],
            &state->installed[index + 1],
            (state->installed_count - (size_t)index - 1) * sizeof(PackageIdentity));
    state->installed_count--;
    return CUP_OK;
}

CupError state_clear_active(CupState *state, const PackageScope *scope) {
    size_t i;

    for (i = 0; i < state->active_count; ++i) {
        PackageIdentity *item = &state->active[i];
        if (strcmp(item->component, scope->component) == 0 &&
            strcmp(item->host_platform, scope->host_platform) == 0 &&
            strcmp(item->target_platform, scope->target_platform) == 0) {
            memmove(item, item + 1, (state->active_count - i - 1) * sizeof(PackageIdentity));
            state->active_count--;
            break;
        }
    }
    return CUP_OK;
}

CupError state_clear_matching_active(CupState *state, const PackageIdentity *identity) {
    size_t i;
    PackageScope scope;

    for (i = 0; i < state->active_count; ++i) {
        if (identities_equal(&state->active[i], identity)) {
            if (package_identity_get_scope(identity, &scope) != CUP_OK) {
                return CUP_ERR_INVALID_INPUT;
            }
            return state_clear_active(state, &scope);
        }
    }
    return CUP_OK;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *package) {
    return snprintf(buffer, size, "/tmp/%s-%s", package->tool, package->version) > 0
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_metadata_is_read_only(const char *base_path, int *is_read_only) {
    (void)base_path;
    *is_read_only = metadata_read_only;
    return CUP_OK;
}

CupError package_set_metadata_read_only(const char *base_path) {
    (void)base_path;
    set_metadata_calls++;
    return CUP_OK;
}

CupError package_quarantine(const PackageIssue *issue, char *recovery_path, size_t recovery_size) {
    TEST_ASSERT_NOT_NULL(issue);
    quarantine_calls++;
    if (quarantine_result != CUP_OK) {
        return quarantine_result;
    }
    return snprintf(recovery_path, recovery_size, "%s.invalid", issue->path) > 0
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

const char *package_issue_reason_name(PackageIssueReason reason) {
    (void)reason;
    return "invalid content";
}

CupError state_save(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    save_calls++;
    return state_save_result;
}

void wrapper_plan_init(WrapperPlan *plan) {
    memset(plan, 0, sizeof(*plan));
}

void wrapper_plan_free(WrapperPlan *plan) {
    memset(plan, 0, sizeof(*plan));
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    TEST_ASSERT_NOT_NULL(plan);
    TEST_ASSERT_NOT_NULL(state);
    plan_build_calls++;
    return plan_build_result;
}

CupError wrapper_plan_apply(const WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_apply_calls++;
    return plan_apply_result;
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/cup-tmp") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError filesystem_clear_directory(const char *directory, const char *preserved_path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/cup-tmp", directory);
    TEST_ASSERT_EQUAL_STRING("/tmp/transaction.txt", preserved_path);
    cleanup_calls++;
    return cleanup_result;
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/common.sum") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/platform.sum") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/cup") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/uninstall.sh") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/packages.cfg") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return snprintf(buffer, size, "/tmp/install.cfg") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_assets_platform_checksums_name(char *name, size_t size) {
    return snprintf(name, size, "SHA256SUMS-linux-x64") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError cup_assets_binary_asset_name(char *name, size_t size) {
    return snprintf(name, size, "cup-linux-x64") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_is_regular_file(const char *path, int *is_regular) {
    if (install_policy_regular_override && strcmp(path, "/tmp/install.cfg") == 0) {
        *is_regular = install_policy_regular;
        return install_policy_regular_result;
    }
    *is_regular = cup_assets_files_regular;
    return CUP_OK;
}

CupError checksum_validate_assets(const char *path,
                                  const char *const *required_assets,
                                  size_t required_count) {
    (void)path;
    (void)required_assets;
    (void)required_count;
    return CUP_OK;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    if (install_policy_read_only_override && strcmp(path, "/tmp/install.cfg") == 0) {
        *is_read_only = install_policy_read_only;
        return install_policy_read_only_result;
    }
    *is_read_only = cup_assets_read_only;
    return CUP_OK;
}

CupError system_is_executable(const char *path, int *is_executable) {
    (void)path;
    *is_executable = cup_assets_executable;
    return CUP_OK;
}

CupError system_set_read_only(const char *path, int read_only) {
    (void)path;
    (void)read_only;
    set_read_only_calls++;
    return CUP_OK;
}

CupError system_set_executable(const char *path, int executable) {
    (void)path;
    (void)executable;
    set_executable_calls++;
    return CUP_OK;
}

CupError cup_assets_verify_asset(const char *checksum_path,
                                 const char *asset_name,
                                 const char *asset_path,
                                 int *matches) {
    (void)checksum_path;
    (void)asset_path;
    if (install_policy_verify_override && strcmp(asset_name, CUP_INSTALL_POLICY_FILENAME) == 0) {
        size_t index = install_policy_verify_calls++;

        TEST_ASSERT_TRUE(index < 2);
        *matches = install_policy_verify_matches[index];
        return install_policy_verify_results[index];
    }
    *matches = verify_matches;
    return CUP_OK;
}

void package_catalog_init(PackageCatalog *catalog) {
    memset(catalog, 0, sizeof(*catalog));
}

void package_catalog_free(PackageCatalog *catalog) {
    memset(catalog, 0, sizeof(*catalog));
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    (void)catalog;
    return CUP_OK;
}

CupError package_catalog_load_path(PackageCatalog *catalog,
                                   const char *path,
                                   PackageCatalogSource source) {
    (void)catalog;
    (void)path;
    (void)source;
    return CUP_OK;
}

void install_policy_init(InstallPolicy *config) {
    memset(config, 0, sizeof(*config));
}

CupError install_policy_load_path(InstallPolicy *config,
                                  const char *path,
                                  InstallPolicySource source) {
    (void)config;
    (void)path;
    (void)source;
    if (install_policy_load_override) {
        size_t index = install_policy_load_calls++;

        TEST_ASSERT_TRUE(index < 2);
        return install_policy_load_results[index];
    }
    return CUP_OK;
}

CupError download_file(const char *url, const char *destination, DownloadValidation validation) {
    (void)destination;
    (void)validation;
    fetch_calls++;
    if (strstr(url, "/" CUP_INSTALL_POLICY_FILENAME) != NULL) {
        return install_policy_fetch_result;
    }
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    (void)directory;
    (void)prefix;
    *file = tmpfile();
    if (*file == NULL) {
        return CUP_ERR_TEMPORARY;
    }
    return snprintf(path, path_size, "/tmp/staged-%d", fetch_calls) > 0 ? CUP_OK
                                                                        : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError system_remove_file(const char *path) {
    (void)path;
    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    (void)path;
    *exists = destination_exists;
    return CUP_OK;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    (void)source;
    (void)destination;
    *commit_state =
        restore_move_result == CUP_OK ? SYSTEM_COMMIT_DURABLE : SYSTEM_COMMIT_NOT_APPLIED;
    return restore_move_result;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *commit_state) {
    (void)source;
    if (install_policy_replace_override && strcmp(destination, "/tmp/install.cfg") == 0) {
        *commit_state = install_policy_replace_state;
        return install_policy_replace_result;
    }
    *commit_state = replace_state;
    return replace_result;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    (void)path;
    return snprintf(hex, size, "0000000000000000000000000000000000000000000000000000000000000000") >
                   0
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

static void test_success_reconciles(void) {
    PackageList *packages = &scan_lists[0];

    loaded_state.installed[loaded_state.installed_count++] = state_entry("clang@1.0.0");
    loaded_state.active[loaded_state.active_count++] = state_entry("clang@1.0.0");
    packages->items[packages->count++] = package_identity("2.0.0");
    packages->total_count = packages->count;
    metadata_read_only = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_INT(1, set_metadata_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_block_invalid_state(void) {
    state_load_result = CUP_ERR_STATE_LOAD;
    transaction_statuses[0] = PACKAGE_TRANSACTION_LOADED;
    transactions[0].operation = PACKAGE_OPERATION_INSTALL;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_repair());
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_preserve_invalid(void) {
    state_load_result = CUP_ERR_STATE_LOAD;
    transaction_results[0] = CUP_ERR_TRANSACTION;
    transaction_results[1] = CUP_ERR_TRANSACTION;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_repair());
    TEST_ASSERT_EQUAL_INT(0, backup_calls);
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);
    TEST_ASSERT_EQUAL_INT(0, save_calls);
}

static void test_recover_transaction(void) {
    transaction_statuses[0] = PACKAGE_TRANSACTION_LOADED;
    transactions[0].operation = PACKAGE_OPERATION_REMOVE;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_INT(1, recover_calls);

    reset_scenario();
    transaction_statuses[0] = PACKAGE_TRANSACTION_LOADED;
    transactions[0].operation = PACKAGE_OPERATION_REMOVE;
    recover_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_repair());
    TEST_ASSERT_EQUAL_INT(0, plan_build_calls);
}

static void test_scan_limits(void) {
    scan_lists[0].complete = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_repair());

    reset_scenario();
    scan_lists[0].total_count = MAX_INSTALLED + 1u;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_repair());
}

static void test_quarantine_rescans(void) {
    PackageIssue *issue = &scan_lists[0].issues[0];

    scan_lists[0].issue_count = 1;
    scan_lists[0].total_issue_count = 1;
    issue->can_quarantine = 1;
    strcpy(issue->path, "/tmp/bad-package");
    scan_lists[1].complete = 1;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_INT(2, (int)scan_calls);
    TEST_ASSERT_EQUAL_INT(1, quarantine_calls);
}

static void test_asset_permissions(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_read_only = 0;
    cup_assets_executable = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_TRUE(set_read_only_calls >= 3);
    TEST_ASSERT_TRUE(set_executable_calls >= 2);
}

static void test_cup_assets_restores(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_files_regular = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_TRUE(fetch_calls >= 6);
    TEST_ASSERT_TRUE(set_read_only_calls >= 4);
    TEST_ASSERT_TRUE(set_executable_calls >= 2);
}

static void test_asset_rollback(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_files_regular = 0;
    destination_exists = 1;
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    restore_move_result = CUP_ERR_FILESYSTEM;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, command_repair());
    TEST_ASSERT_TRUE(backup_calls >= 1);
}

static void test_asset_restore(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_files_regular = 0;
    destination_exists = 1;
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    restore_move_result = CUP_OK;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
    TEST_ASSERT_TRUE(backup_calls >= 1);
}

static void prepare_installed_official_cup_assets(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_files_regular = 1;
}

static void test_config_repair(void) {
    prepare_installed_official_cup_assets();
    install_policy_read_only_override = 1;
    install_policy_read_only_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());

    reset_scenario();
    prepare_installed_official_cup_assets();
    install_policy_load_override = 1;
    install_policy_load_results[0] = CUP_ERR_VALIDATION;
    install_policy_load_results[1] = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_repair());
    TEST_ASSERT_EQUAL_UINT(2, install_policy_load_calls);

    reset_scenario();
    prepare_installed_official_cup_assets();
    install_policy_verify_override = 1;
    install_policy_verify_matches[0] = 0;
    install_policy_verify_matches[1] = 1;
    install_policy_fetch_result = CUP_ERR_FETCH;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_repair());

    reset_scenario();
    prepare_installed_official_cup_assets();
    install_policy_verify_override = 1;
    install_policy_verify_matches[0] = 0;
    install_policy_verify_matches[1] = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_repair());

    reset_scenario();
    prepare_installed_official_cup_assets();
    install_policy_verify_override = 1;
    install_policy_verify_matches[0] = 0;
    install_policy_verify_matches[1] = 1;
    install_policy_load_override = 1;
    install_policy_load_results[0] = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_repair());

    reset_scenario();
    prepare_installed_official_cup_assets();
    install_policy_verify_override = 1;
    install_policy_verify_matches[0] = 0;
    install_policy_verify_matches[1] = 1;
    install_policy_replace_override = 1;
    install_policy_replace_result = CUP_ERR_FILESYSTEM;
    install_policy_replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
}

static void test_cup_assets_rejects(void) {
    has_installed_assets = 1;
    development_valid = 0;
    cup_assets_files_regular = 0;
    verify_matches = 0;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_repair());
    TEST_ASSERT_EQUAL_INT(0, plan_build_calls);
}

static void test_early_failures(void) {
    root_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
    TEST_ASSERT_EQUAL_INT(0, lock_release_calls);

    reset_scenario();
    lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_repair());
    TEST_ASSERT_EQUAL_INT(0, lock_release_calls);

    reset_scenario();
    pending_uninstall = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_repair());
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    runtime_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_late_failures(void) {
    plan_build_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_repair());
    TEST_ASSERT_EQUAL_INT(0, plan_apply_calls);

    reset_scenario();
    plan_apply_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
    TEST_ASSERT_EQUAL_INT(0, cleanup_calls);

    reset_scenario();
    cleanup_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_repair());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_success_reconciles);
    RUN_TEST(test_block_invalid_state);
    RUN_TEST(test_preserve_invalid);
    RUN_TEST(test_recover_transaction);
    RUN_TEST(test_scan_limits);
    RUN_TEST(test_quarantine_rescans);
    RUN_TEST(test_asset_permissions);
    RUN_TEST(test_cup_assets_restores);
    RUN_TEST(test_asset_rollback);
    RUN_TEST(test_asset_restore);
    RUN_TEST(test_config_repair);
    RUN_TEST(test_cup_assets_rejects);
    RUN_TEST(test_early_failures);
    RUN_TEST(test_late_failures);
    return UNITY_END();
}
