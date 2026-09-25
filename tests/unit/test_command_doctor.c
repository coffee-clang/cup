/*
 * Exercises doctor decisions with all external inspections simulated. Integration
 * tests own real filesystem diagnosis.
 */

#include "generation.h"
#include "commands.h"
#include "package_selector.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "update_journal.h"
#include "runtime_journal.h"
#include "uninstall_journal.h"
#include "wrappers.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CupError root_candidates_result;
    CupError root_snapshot_result;
    size_t root_issue_count;
    CupError generation_result;
    GenerationInspection generation;
    CupError package_catalog_result;
    CupError root_path_result;
    CupError root_kind_result;
    SystemPathKind root_kind;
    CupError lock_path_result;
    CupError lock_file_result;
    int lock_exists;
    CupError lock_result;
    CupError runtime_check_result;
    size_t missing_count;
    CupError state_result;
    StateFileStatus state_status;
    int include_state_package;
    CupError journal_result;
    RuntimeJournalKind journal_kind;
    CupError transaction_result;
    PackageTransactionStatus transaction_status;
    PackageOperation transaction_operation;
    CupError update_load_result;
    UpdateJournalStatus update_status;
    CupError uninstall_load_result;
    UninstallJournalStatus uninstall_status;
    UninstallPhase uninstall_phase;
    int uninstall_error;
    CupError identity_result;
    CupError install_path_result;
    CupError package_result;
    CupError package_catalog_check_result;
    int package_catalog_available;
    CupError plan_build_result;
    CupError plan_check_result;
    size_t wrapper_issues;
    CupError scan_result;
    PackageList packages;
    CupError tmp_path_result;
    CupError transaction_path_result;
    CupError tmp_directory_result;
    int tmp_exists;
    CupError tmp_count_result;
    size_t tmp_count;
    CupError read_only_result;
    int read_only;
} DoctorScenario;

static DoctorScenario scenario;
static PackageIdentity scenario_package_items[16];
static int generation_inspect_calls;
static int root_snapshot_begin_calls;
static int root_snapshot_end_calls;
static int lock_release_calls;
static int plan_free_calls;
static int runtime_check_calls;
static int package_catalog_check_calls;
static int plan_build_calls;
static int tmp_count_calls;

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void fill_identity(PackageIdentity *package, const char *version) {
    memset(package, 0, sizeof(*package));
    (void)snprintf(package->component, sizeof(package->component), "compiler");
    (void)snprintf(package->tool, sizeof(package->tool), "clang");
    (void)snprintf(package->host_platform, sizeof(package->host_platform), "linux-x64");
    (void)snprintf(package->target_platform, sizeof(package->target_platform), "linux-x64");
    (void)snprintf(package->version, sizeof(package->version), "%s", version);
}

static void reset_scenario(void) {
    memset(&scenario, 0, sizeof(scenario));
    memset(scenario_package_items, 0, sizeof(scenario_package_items));
    scenario.packages.items = scenario_package_items;
    scenario.packages.capacity = 16;
    scenario.generation.release = CUP_GENERATION_ASSET_VALID;
    scenario.generation.license = CUP_GENERATION_ASSET_VALID;
    scenario.generation.notices = CUP_GENERATION_ASSET_VALID;
    scenario.generation.binary = CUP_GENERATION_ASSET_VALID;
    scenario.root_kind = SYSTEM_PATH_DIRECTORY;
    scenario.lock_exists = 1;
    scenario.state_status = STATE_FILE_LOADED;
    scenario.include_state_package = 1;
    scenario.journal_kind = RUNTIME_JOURNAL_MISSING;
    scenario.transaction_status = PACKAGE_TRANSACTION_MISSING;
    scenario.update_status = CUP_UPDATE_JOURNAL_MISSING;
    scenario.uninstall_status = UNINSTALL_JOURNAL_MISSING;
    scenario.uninstall_phase = UNINSTALL_PHASE_SCHEDULED;
    scenario.package_catalog_available = 1;
    scenario.packages.complete = 1;
    scenario.tmp_exists = 1;
    scenario.read_only = 1;
    fill_identity(&scenario.packages.items[0], "22.1.5");
    scenario.packages.count = 1;
    scenario.packages.total_count = 1;
    generation_inspect_calls = 0;
    root_snapshot_begin_calls = 0;
    root_snapshot_end_calls = 0;
    lock_release_calls = 0;
    plan_free_calls = 0;
    runtime_check_calls = 0;
    package_catalog_check_calls = 0;
    plan_build_calls = 0;
    tmp_count_calls = 0;
}

CupError platform_get_host(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "linux-x64"), size);
}

size_t state_count_foreign_hosts(const CupState *state, const char *current_host) {
    (void)state;
    (void)current_host;
    return 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

void package_catalog_init(PackageCatalog *catalog) {
    memset(catalog, 0, sizeof(*catalog));
}

void package_catalog_free(PackageCatalog *catalog) {
    (void)catalog;
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    (void)catalog;
    return scenario.package_catalog_result;
}

CupError package_catalog_load_development(PackageCatalog *catalog) {
    (void)catalog;
    return scenario.package_catalog_result;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host,
                                     const char *target,
                                     const char *version,
                                     int *available) {
    package_catalog_check_calls++;
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host;
    (void)target;
    (void)version;
    if (available != NULL) {
        *available = scenario.package_catalog_available;
    }
    return scenario.package_catalog_check_result;
}

CupError generation_inspect(GenerationInspection *inspection) {
    generation_inspect_calls++;
    if (inspection != NULL) *inspection = scenario.generation;
    return scenario.generation_result;
}

int generation_has_installed_assets(const GenerationInspection *inspection) {
    return inspection->release != CUP_GENERATION_ASSET_MISSING ||
           inspection->license != CUP_GENERATION_ASSET_MISSING ||
           inspection->notices != CUP_GENERATION_ASSET_MISSING ||
           inspection->binary != CUP_GENERATION_ASSET_MISSING;
}

int generation_installed_is_valid(const GenerationInspection *inspection) {
    return inspection->release == CUP_GENERATION_ASSET_VALID &&
           inspection->license == CUP_GENERATION_ASSET_VALID &&
           inspection->notices == CUP_GENERATION_ASSET_VALID &&
           inspection->binary == CUP_GENERATION_ASSET_VALID;
}

CupError generation_asset_specs(GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT]) {
    size_t i;
    static const char *names[CUP_GENERATION_ASSET_COUNT] = {
        "release.txt", "LICENSE", "THIRD_PARTY_NOTICES.txt", "cup-linux-x64"
    };
    static const char *paths[CUP_GENERATION_ASSET_COUNT] = {
        "/doctor/release.txt", "/doctor/LICENSE",
        "/doctor/THIRD_PARTY_NOTICES.txt", "/doctor/bin/cup"
    };
    for (i = 0; i < CUP_GENERATION_ASSET_COUNT; ++i) {
        memset(&specs[i], 0, sizeof(specs[i]));
        specs[i].id = (GenerationAssetId)i;
        strcpy(specs[i].release_name, names[i]);
        strcpy(specs[i].destination, paths[i]);
        specs[i].read_only = i != CUP_GENERATION_ASSET_BINARY;
        specs[i].executable = i == CUP_GENERATION_ASSET_BINARY;
    }
    return CUP_OK;
}

static CupError copy_path(char *buffer, size_t size, const char *name, CupError result) {
    if (result != CUP_OK) {
        return result;
    }
    return buffer_write_result(snprintf(buffer, size, "/doctor/%s", name), size);
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "catalog", CUP_OK);
}







CupError layout_get_root(char *buffer, size_t size) {
    return copy_path(buffer, size, "root", scenario.root_path_result);
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "cup.lock", scenario.lock_path_result);
}

CupError layout_get_bin_dir(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "/test/.cup/bin"), size);
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return copy_path(buffer, size, "tmp", scenario.tmp_path_result);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return copy_path(buffer, size, "transaction", scenario.transaction_path_result);
}

CupError layout_root_snapshot_begin(void) {
    root_snapshot_begin_calls++;
    return scenario.root_snapshot_result;
}

void layout_root_snapshot_end(void) {
    root_snapshot_end_calls++;
}

CupError layout_root_snapshot_validate(void) {
    return CUP_OK;
}

CupError layout_check_root_candidates(size_t *issue_count) {
    if (issue_count != NULL) {
        *issue_count = scenario.root_issue_count;
    }
    return scenario.root_candidates_result;
}

CupError layout_check_runtime(size_t *missing_count) {
    runtime_check_calls++;
    if (missing_count != NULL) {
        *missing_count = scenario.missing_count;
    }
    return scenario.runtime_check_result;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    (void)identity;
    return copy_path(buffer, size, "package", scenario.install_path_result);
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    (void)path;
    if (kind == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *kind = scenario.root_kind;
    return scenario.root_kind_result;
}

CupError system_is_read_only(const char *path, int *is_read_only) {
    (void)path;
    if (is_read_only != NULL) {
        *is_read_only = scenario.read_only;
    }
    return scenario.read_only_result;
}

CupError system_is_regular_file(const char *path, int *is_regular) {
    (void)path;
    if (is_regular != NULL) {
        *is_regular = scenario.lock_exists;
    }
    return scenario.lock_file_result;
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    (void)path;
    if (scenario.lock_result == CUP_OK && lock != NULL) {
        lock->handle = 7;
        lock->mode = mode;
        lock->active = 1;
    }
    return scenario.lock_result;
}

void system_lock_release(SystemLock *lock) {
    if (lock != NULL && lock->active) {
        lock->active = 0;
        lock->mode = SYSTEM_LOCK_SHARED;
    }
    lock_release_calls++;
}

CupError system_is_directory(const char *path, int *is_directory) {
    (void)path;
    if (is_directory != NULL) {
        *is_directory = scenario.tmp_exists;
    }
    return scenario.tmp_directory_result;
}

CupError filesystem_count_children(const char *path, const char *excluded, size_t *count) {
    tmp_count_calls++;
    (void)path;
    (void)excluded;
    if (count != NULL) {
        *count = scenario.tmp_count;
    }
    return scenario.tmp_count_result;
}

void state_init(CupState *state) {
    if (state != NULL) memset(state, 0, sizeof(*state));
}

void state_free(CupState *state) {
    if (state == NULL) return;
    free(state->installed);
    memset(state, 0, sizeof(*state));
}

CupError state_load(CupState *state,
                    StateFileStatus *status,
                    SystemPathIdentity *source_identity,
                    FILE *diagnostics) {
    TEST_ASSERT_NULL(source_identity);
    TEST_ASSERT_NULL(diagnostics);
    state_free(state);
    state_init(state);
    *status = scenario.state_status;
    if (scenario.state_result != CUP_OK) {
        return scenario.state_result;
    }
    if (scenario.include_state_package) {
        state->installed = calloc(1, sizeof(*state->installed));
        TEST_ASSERT_NOT_NULL(state->installed);
        state->installed_capacity = 1;
        state->installed_count = 1;
        (void)snprintf(
            state->installed[0].component, sizeof(state->installed[0].component), "compiler");
        (void)snprintf(state->installed[0].host_platform,
                       sizeof(state->installed[0].host_platform),
                       "linux-x64");
        (void)snprintf(state->installed[0].target_platform,
                       sizeof(state->installed[0].target_platform),
                       "linux-x64");
        (void)snprintf(state->installed[0].tool, sizeof(state->installed[0].tool), "clang");
        (void)snprintf(state->installed[0].version, sizeof(state->installed[0].version), "22.1.5");
    }
    return CUP_OK;
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    (void)identity;
    return state->installed_count > 0 ? 0 : -1;
}

CupError runtime_journal_detect(RuntimeJournalKind *kind) {
    *kind = scenario.journal_kind;
    return scenario.journal_result;
}

void update_journal_init(UpdateJournal *journal) {
    memset(journal, 0, sizeof(*journal));
}

CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status) {
    update_journal_init(journal);
    *status = scenario.update_status;
    (void)snprintf(journal->temporary_name, sizeof(journal->temporary_name), "cup-update-abc");
    (void)snprintf(journal->target_release_sha256, sizeof(journal->target_release_sha256),
                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    return scenario.update_load_result;
}

void uninstall_journal_init(UninstallJournal *journal) {
    memset(journal, 0, sizeof(*journal));
}

CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status) {
    uninstall_journal_init(journal);
    *status = scenario.uninstall_status;
    journal->phase = scenario.uninstall_phase;
    journal->error_code = scenario.uninstall_error;
    return scenario.uninstall_load_result;
}

const char *uninstall_phase_name(UninstallPhase phase) {
    switch (phase) {
        case UNINSTALL_PHASE_SCHEDULED: return "scheduled";
        case UNINSTALL_PHASE_DETACHING: return "detaching";
        case UNINSTALL_PHASE_FAILED: return "failed";
        default: return "invalid";
    }
}


void package_transaction_init(PackageTransaction *transaction) {
    memset(transaction, 0, sizeof(*transaction));
}

CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status) {
    *status = scenario.transaction_status;
    transaction->operation = scenario.transaction_operation;
    fill_identity(&transaction->package, "22.1.5");
    return scenario.transaction_result;
}

const char *package_operation_name(PackageOperation operation) {
    return operation == PACKAGE_OPERATION_REMOVE ? "remove" : "install";
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host,
                                        const char *target,
                                        const char *entry,
                                        FILE *diagnostics) {
    (void)diagnostics;
    (void)component;
    (void)host;
    (void)target;
    (void)entry;
    if (scenario.identity_result == CUP_OK) {
        fill_identity(identity, "22.1.5");
    }
    return scenario.identity_result;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    if (identity == NULL || buffer == NULL) {
        return CUP_ERR_VALIDATION;
    }
    return buffer_write_result(
        snprintf(buffer, size, "%s@%s", identity->tool, identity->version), size);
}

CupError package_validate(const char *path, const PackageIdentity *identity, FILE *diagnostics) {
    TEST_ASSERT_NULL(diagnostics);
    (void)path;
    (void)identity;
    return scenario.package_result;
}

CupError package_scan(PackageList *packages, FILE *diagnostics) {
    TEST_ASSERT_NULL(diagnostics);
    if (scenario.scan_result == CUP_OK) {
        *packages = scenario.packages;
    }
    return scenario.scan_result;
}

void package_list_init(PackageList *packages) {
    TEST_ASSERT_NOT_NULL(packages);
    memset(packages, 0, sizeof(*packages));
}

void package_list_free(PackageList *packages) {
    TEST_ASSERT_NOT_NULL(packages);
    memset(packages, 0, sizeof(*packages));
}

const char *package_issue_reason_name(PackageIssueReason reason) {
    (void)reason;
    return "invalid content";
}

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *version) {
    return buffer_write_result(snprintf(buffer, size, "%s@%s", tool, version), size);
}

void wrapper_plan_init(WrapperPlan *plan) {
    memset(plan, 0, sizeof(*plan));
}

void wrapper_plan_free(WrapperPlan *plan) {
    (void)plan;
    plan_free_calls++;
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    plan_build_calls++;
    (void)plan;
    (void)state;
    return scenario.plan_build_result;
}

CupError wrapper_plan_check(const WrapperPlan *plan, size_t *issue_count) {
    (void)plan;
    if (issue_count != NULL) {
        *issue_count = scenario.wrapper_issues;
    }
    return scenario.plan_check_result;
}

static void test_healthy(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_doctor());
    TEST_ASSERT_EQUAL_INT(1, root_snapshot_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, root_snapshot_end_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_free_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_generation_modes(void) {
    scenario.root_kind = SYSTEM_PATH_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_doctor());
    TEST_ASSERT_EQUAL_INT(0, generation_inspect_calls);

    reset_scenario();
    scenario.generation_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_generation_issues(void) {
    scenario.generation.binary = CUP_GENERATION_ASSET_INVALID;
    scenario.generation.license = CUP_GENERATION_ASSET_MISSING;
    scenario.read_only = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.package_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_runtime_gates(void) {
    scenario.lock_exists = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.runtime_check_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
    TEST_ASSERT_EQUAL_INT(0, generation_inspect_calls);

    reset_scenario();
    scenario.root_kind = SYSTEM_PATH_MISSING;
    scenario.root_kind = SYSTEM_PATH_DIRECTORY;
    scenario.lock_exists = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
    TEST_ASSERT_EQUAL_INT(0, generation_inspect_calls);
}

static void test_root_and_uninstall_journal(void) {
    scenario.root_issue_count = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
    TEST_ASSERT_EQUAL_INT(0, root_snapshot_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, runtime_check_calls);

    reset_scenario();
    scenario.root_snapshot_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
    TEST_ASSERT_EQUAL_INT(1, root_snapshot_begin_calls);
    TEST_ASSERT_EQUAL_INT(0, root_snapshot_end_calls);
    TEST_ASSERT_EQUAL_INT(0, runtime_check_calls);

    reset_scenario();
    scenario.journal_kind = RUNTIME_JOURNAL_UNINSTALL;
    scenario.uninstall_status = UNINSTALL_JOURNAL_LOADED;
    scenario.uninstall_phase = UNINSTALL_PHASE_FAILED;
    scenario.uninstall_error = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.journal_kind = RUNTIME_JOURNAL_UNINSTALL;
    scenario.uninstall_load_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_state_issues(void) {
    scenario.state_result = CUP_ERR_VALIDATION;
    scenario.journal_kind = RUNTIME_JOURNAL_GENERATION;
    scenario.update_status = CUP_UPDATE_JOURNAL_LOADED;
    scenario.packages.complete = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.state_result = CUP_ERR_STATE_LOAD;
    scenario.journal_result = CUP_ERR_VALIDATION;
    scenario.packages.issue_count = 1;
    scenario.packages.total_issue_count = 3;
    (void)snprintf(
        scenario.packages.issues[0].path, sizeof(scenario.packages.issues[0].path), "/bad/package");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_package_issues(void) {
    scenario.package_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.wrapper_issues = 2;
    scenario.include_state_package = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.package_result = CUP_ERR_FILESYSTEM;
    scenario.scan_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_update_journal(void) {
    scenario.journal_kind = RUNTIME_JOURNAL_GENERATION;
    scenario.update_status = CUP_UPDATE_JOURNAL_LOADED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());

    reset_scenario();
    scenario.journal_kind = RUNTIME_JOURNAL_GENERATION;
    scenario.update_load_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
}

static void test_warning_only(void) {
    scenario.package_catalog_available = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_doctor());
    TEST_ASSERT_EQUAL_INT(1, package_catalog_check_calls);

    reset_scenario();
    scenario.tmp_count = 2;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_doctor());
    TEST_ASSERT_EQUAL_INT(1, tmp_count_calls);
}

static void test_incomplete_checks(void) {
    scenario.runtime_check_result = CUP_ERR_FILESYSTEM;
    scenario.package_catalog_check_result = CUP_ERR_VALIDATION;
    scenario.plan_build_result = CUP_ERR_VALIDATION;
    scenario.tmp_count_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_doctor());
    TEST_ASSERT_EQUAL_INT(1, runtime_check_calls);
    TEST_ASSERT_EQUAL_INT(1, package_catalog_check_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, tmp_count_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_free_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_healthy);
    RUN_TEST(test_generation_modes);
    RUN_TEST(test_generation_issues);
    RUN_TEST(test_runtime_gates);
    RUN_TEST(test_root_and_uninstall_journal);
    RUN_TEST(test_state_issues);
    RUN_TEST(test_package_issues);
    RUN_TEST(test_update_journal);
    RUN_TEST(test_warning_only);
    RUN_TEST(test_incomplete_checks);
    return UNITY_END();
}
