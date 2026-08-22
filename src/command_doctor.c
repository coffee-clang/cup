/*
 * Performs read-only diagnosis of cup assets, runtime layout, state, packages, transactions and
 * managed wrappers. Failed inspections are reported as incomplete rather than silently ignored.
 */

#include "commands.h"

#include "assets.h"
#include "package_selector.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "update_journal.h"
#include "runtime_journal.h"
#include "uninstall_journal.h"

#include <stdio.h>

/* Aggregated diagnostic state; every failed inspection remains visible. */
typedef struct {
    int issue_count;
    int warning_count;
    int incomplete_count;
} DoctorReport;

static int state_contains_package(const CupState *state, const PackageIdentity *package) {
    return state_find_installed(state, package) != -1;
}

static void report_incomplete(DoctorReport *report, const char *description) {
    printf("Incomplete: %s could not be checked.\n", description);
    report->incomplete_count++;
}

static void report_asset_status(DoctorReport *report,
                                const char *description,
                                AssetStatus status) {
    if (status == CUP_ASSET_VALID) {
        printf("OK: %s is valid.\n", description);
        return;
    }

    printf("Issue: %s is %s.\n", description, status == CUP_ASSET_MISSING ? "missing" : "invalid");
    report->issue_count++;
}

static void report_update_helper_status(DoctorReport *report, AssetStatus status) {
    if (status == CUP_ASSET_VALID) {
        printf("OK: derived native update helper is available.\n");
    } else if (status == CUP_ASSET_MISSING) {
        printf("Warning: derived native update helper is missing; it will be recreated "
               "before the next cup update.\n");
        report->warning_count++;
    } else {
        printf("Issue: derived native update helper path is invalid and blocks "
               "regeneration.\n");
        report->issue_count++;
    }
}

static void check_read_only_path(const char *path, const char *description, DoctorReport *report) {
    int is_read_only;

    if (system_is_read_only(path, &is_read_only) != CUP_OK) {
        report_incomplete(report, description);
    } else if (!is_read_only) {
        printf("Issue: %s is not read-only.\n", description);
        report->issue_count++;
    }
}

static CupError load_diagnostic_catalog(const AssetsInspection *inspection,
                                        PackageCatalog *catalog,
                                        int *has_catalog) {
    CupError err;

    *has_catalog = 0;
    if (inspection->catalog == CUP_ASSET_VALID) {
        err = package_catalog_load_installed(catalog);
    } else if (inspection->development_catalog_valid) {
        err = package_catalog_load_development(catalog);
        if (err == CUP_OK) {
            printf("Info: using the development catalog only for "
                   "additional diagnostics.\n");
        }
    } else {
        return CUP_OK;
    }

    if (err == CUP_OK) {
        *has_catalog = 1;
    }
    return err;
}

static CupError check_assets(PackageCatalog *catalog, DoctorReport *report, int *has_catalog) {
    AssetsInspection inspection;
    CupError err;
    char path[MAX_PATH_LEN];

    err = assets_inspect(&inspection);
    if (err != CUP_OK) {
        report_incomplete(report, "cup assets");
        return CUP_OK;
    }

    if (assets_has_installed_assets(&inspection)) {
        report_asset_status(report, "installed cup executable", inspection.binary);
        report_update_helper_status(report, inspection.helper);
        report_asset_status(report, "installed package catalog", inspection.catalog);
        report_asset_status(report, "installation configuration", inspection.install_policy);
        report_asset_status(report, "common checksum file", inspection.common_checksums);
        report_asset_status(report, "platform checksum file", inspection.platform_checksums);

        if (inspection.catalog == CUP_ASSET_VALID &&
            layout_get_package_catalog_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "installed package catalog", report);
        }
        if (inspection.install_policy == CUP_ASSET_VALID &&
            layout_get_install_policy_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "installation configuration", report);
        }
        if (inspection.common_checksums == CUP_ASSET_VALID &&
            layout_get_common_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "common checksum file", report);
        }
        if (inspection.platform_checksums == CUP_ASSET_VALID &&
            layout_get_platform_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "platform checksum file", report);
        }
    } else if (assets_development_is_valid(&inspection)) {
        printf("OK: development cup assets are available.\n");
    } else {
        printf("Issue: neither installed nor development cup assets "
               "are complete and valid.\n");
        report->issue_count++;
    }

    err = load_diagnostic_catalog(&inspection, catalog, has_catalog);
    if (err != CUP_OK) {
        report_incomplete(report, "current package catalog");
    }
    return CUP_OK;
}

static void check_state_packages(const CupState *state,
                                 const PackageCatalog *catalog,
                                 int has_catalog,
                                 DoctorReport *report) {
    size_t i;

    /* Reconstruct and validate each installed package before checking secondary properties. */
    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *package = &state->installed[i];
        char selector[MAX_SELECTOR_LEN] = "(invalid identity)";
        char install_path[MAX_PATH_LEN];
        int is_read_only;
        int is_available;
        CupError err;

        if (package_identity_format_selector(package, selector, sizeof(selector)) != CUP_OK) {
            printf("Issue: installed state record %zu is invalid.\n", i + 1);
            report->issue_count++;
            continue;
        }

        err = layout_build_install_path(install_path, sizeof(install_path), package);
        if (err != CUP_OK) {
            printf("Incomplete: package path for '%s:%s' could not "
                   "be constructed.\n",
                   package->component,
                   selector);
            report->incomplete_count++;
            continue;
        }

        err = package_validate(install_path, package, NULL);
        if (err == CUP_ERR_VALIDATION) {
            printf("Issue: installed state record '%s:%s' has no "
                   "valid package.\n",
                   package->component,
                   selector);
            report->issue_count++;
            continue;
        }
        if (err != CUP_OK) {
            printf("Incomplete: package '%s:%s' could not be inspected.\n",
                   package->component,
                   selector);
            report->incomplete_count++;
            continue;
        }

        /* Metadata protection is diagnostic and does not suppress catalog checks. */
        err = package_metadata_is_read_only(install_path, &is_read_only);
        if (err != CUP_OK) {
            printf("Incomplete: package metadata protection for '%s:%s' "
                   "could not be checked.\n",
                   package->component,
                   selector);
            report->incomplete_count++;
        } else if (!is_read_only) {
            printf("Issue: package metadata for '%s:%s' is not read-only.\n",
                   package->component,
                   selector);
            report->issue_count++;
        }

        /* Catalog availability is a warning because installed concrete versions remain usable. */
        if (has_catalog) {
            err = package_catalog_has_version(catalog,
                                              package->component,
                                              package->tool,
                                              package->host_platform,
                                              package->target_platform,
                                              package->version,
                                              &is_available);

            if (err != CUP_OK) {
                printf("Incomplete: catalog availability for '%s:%s' "
                       "could not be checked.\n",
                       package->component,
                       selector);
                report->incomplete_count++;
            } else if (!is_available) {
                printf("Warning: installed package '%s:%s' is not listed "
                       "by the current catalog.\n",
                       package->component,
                       selector);
                report->warning_count++;
            }
        }
    }
}

static void check_scanned_packages(const PackageList *packages,
                                   const CupState *state,
                                   int state_loaded,
                                   DoctorReport *report) {
    size_t i;

    if (packages->foreign_host_count > 0) {
        printf("Warning: preserved %zu foreign-host package tree(s) without inspecting or adopting "
               "them.\n",
               packages->foreign_host_count);
        report->warning_count++;
    }

    if (!packages->complete) {
        printf("Issue: package scan exceeded its in-memory capacity and is incomplete.\n");
        report->issue_count++;
    }

    for (i = 0; i < packages->issue_count; ++i) {
        printf("Issue: package path '%s' is invalid: %s.\n",
               packages->issues[i].path,
               package_issue_reason_name(packages->issues[i].reason));
        report->issue_count++;
    }

    if (packages->total_issue_count > packages->issue_count) {
        size_t omitted = packages->total_issue_count - packages->issue_count;

        printf("Issue: %zu additional invalid package path(s) could not be listed.\n", omitted);
        report->issue_count++;
    }

    if (!state_loaded || !packages->complete) {
        return;
    }

    for (i = 0; i < packages->count; ++i) {
        if (!state_contains_package(state, &packages->items[i])) {
            printf("Issue: valid package '%s@%s' exists in components "
                   "but is absent from state.txt.\n",
                   packages->items[i].tool,
                   packages->items[i].version);
            report->issue_count++;
        }
    }
}

/* Final report and exit-status selection. */
static CupError print_doctor_summary(const DoctorReport *report) {
    if (report->incomplete_count > 0) {
        printf("Doctor found %d issue(s), %d warning(s), and %d "
               "incomplete check(s).\n",
               report->issue_count,
               report->warning_count,
               report->incomplete_count);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (report->issue_count == 0 && report->warning_count == 0) {
        printf("Doctor found no issues.\n");
        return CUP_OK;
    }
    if (report->issue_count == 0) {
        printf("Doctor found %d warning(s), but no blocking issues.\n", report->warning_count);
        return CUP_OK;
    }

    printf("Doctor found %d issue(s) and %d warning(s). "
           "Run 'cup repair' after reviewing them.\n",
           report->issue_count,
           report->warning_count);
    return CUP_ERR_INCONSISTENT_STATE;
}

typedef enum {
    DOCTOR_RUNTIME_MISSING = 0,
    DOCTOR_RUNTIME_ACQUIRED,
    DOCTOR_RUNTIME_UNAVAILABLE
} DoctorRuntimeSnapshot;

/* Runtime snapshot preparation. Diagnostics continue only after a shared lock protects one
 * coherent view of state, journals and installed packages. */
static DoctorRuntimeSnapshot acquire_runtime_snapshot(DoctorReport *report, SystemLock *lock) {
    LayoutRuntimeStatus runtime_status = LAYOUT_RUNTIME_MISSING;
    CupError err;
    char path[MAX_PATH_LEN];
    int lock_exists = 0;

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        report_incomplete(report, "runtime structure");
        return DOCTOR_RUNTIME_UNAVAILABLE;
    }
    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        SystemPathKind root_kind = SYSTEM_PATH_MISSING;

        err = layout_get_root(path, sizeof(path));
        if (err == CUP_OK) {
            err = system_get_path_kind(path, &root_kind);
        }
        if (err != CUP_OK) {
            report_incomplete(report, "cup root");
            return DOCTOR_RUNTIME_UNAVAILABLE;
        }
        if (root_kind == SYSTEM_PATH_MISSING) {
            printf("Info: cup runtime is not initialized; "
                   "the first operational command will create it.\n");
            return DOCTOR_RUNTIME_MISSING;
        }
        if (root_kind != SYSTEM_PATH_DIRECTORY) {
            printf("Issue: cup root is not a real directory.\n");
            report->issue_count++;
            return DOCTOR_RUNTIME_UNAVAILABLE;
        }
    }
    if (runtime_status != LAYOUT_RUNTIME_READY) {
        printf("Issue: cup runtime structure is incomplete.\n");
        report->issue_count++;
    }

    err = layout_get_lock_path(path, sizeof(path));
    if (err == CUP_OK) {
        err = system_is_regular_file(path, &lock_exists);
    }
    if (err != CUP_OK) {
        report_incomplete(report, "lock file");
        return DOCTOR_RUNTIME_UNAVAILABLE;
    }
    if (!lock_exists) {
        printf("Issue: cup lock file is missing: %s\n", path);
        report->issue_count++;
        report_incomplete(report, "coherent runtime snapshot");
        return DOCTOR_RUNTIME_UNAVAILABLE;
    }

    err = system_lock_acquire(lock, path, SYSTEM_LOCK_SHARED);
    if (err == CUP_OK) {
        err = layout_root_snapshot_validate();
        if (err != CUP_OK) {
            system_lock_release(lock);
        }
    }
    if (err != CUP_OK) {
        printf("Issue: %s.\n",
               err == CUP_ERR_LOCK ? "another cup operation is currently running"
                                   : "cup lock could not be acquired");
        report->issue_count++;
        return DOCTOR_RUNTIME_UNAVAILABLE;
    }
    return DOCTOR_RUNTIME_ACQUIRED;
}

static void check_runtime_contents(DoctorReport *report,
                                   char *current_host,
                                   size_t current_host_size) {
    CupError err;
    size_t missing_count = 0;

    err = platform_get_host(current_host, current_host_size);
    if (err != CUP_OK) {
        report_incomplete(report, "current host platform");
        current_host[0] = '\0';
    }

    err = layout_check_runtime(&missing_count);
    if (err != CUP_OK) {
        report_incomplete(report, "runtime contents");
    } else {
        report->issue_count += (int)missing_count;
    }
}

static void load_and_check_state(CupState *state,
                                 const char *current_host,
                                 DoctorReport *report,
                                 int *state_loaded,
                                 int *state_valid) {
    StateFileStatus state_status = STATE_FILE_MISSING;
    CupError err;

    *state_loaded = 0;
    *state_valid = 0;
    err = state_load(state, &state_status, NULL, NULL);
    if (err != CUP_OK) {
        printf("Issue: state.txt is syntactically invalid.\n");
        report->issue_count++;
        return;
    }
    if (state_status == STATE_FILE_MISSING) {
        printf("Issue: state.txt is missing.\n");
        report->issue_count++;
        return;
    }

    *state_loaded = 1;
    if (state_validate(state, NULL) != CUP_OK) {
        printf("Issue: state.txt is semantically inconsistent.\n");
        report->issue_count++;
        return;
    }

    {
        size_t foreign_records = state_count_foreign_hosts(state, current_host);

        *state_valid = 1;
        printf("OK: state.txt is structurally valid.\n");
        if (foreign_records > 0) {
            printf("Warning: state.txt preserves %zu record(s) for foreign hosts; operational "
                   "commands will not manage them.\n",
                   foreign_records);
            report->warning_count++;
        }
    }
}

static void check_transaction_journal(DoctorReport *report) {
    PackageTransaction package_transaction;
    PackageTransactionStatus package_status;
    UpdateJournal update_journal;
    UpdateJournalStatus update_status;
    UninstallJournal uninstall_journal;
    UninstallJournalStatus uninstall_status;
    RuntimeJournalKind journal_kind;
    CupError err;

    package_transaction_init(&package_transaction);
    update_journal_init(&update_journal);
    uninstall_journal_init(&uninstall_journal);

    err = runtime_journal_detect(&journal_kind);
    if (err != CUP_OK) {
        printf("Issue: transaction journal is invalid.\n");
        report->issue_count++;
        return;
    }

    if (journal_kind == RUNTIME_JOURNAL_PACKAGE) {
        err = package_transaction_load(&package_transaction, &package_status);
        if (err != CUP_OK || package_status != PACKAGE_TRANSACTION_LOADED) {
            printf("Issue: package transaction journal is invalid.\n");
        } else {
            printf("Issue: interrupted %s transaction detected for %s@%s.\n",
                   package_operation_name(package_transaction.operation),
                   package_transaction.package.tool,
                   package_transaction.package.version);
        }
        report->issue_count++;
    } else if (journal_kind == RUNTIME_JOURNAL_UPDATE) {
        err = update_journal_load(&update_journal, &update_status);
        if (err != CUP_OK || update_status != CUP_UPDATE_JOURNAL_LOADED) {
            printf("Issue: cup update journal is invalid.\n");
        } else if (update_journal.phase == CUP_UPDATE_PHASE_FAILED) {
            printf("Issue: the previous cup update to version %s failed with error %d; "
                   "recovery is %s.\n",
                   update_journal.version,
                   update_journal.error_code,
                   update_failure_recovery_name(update_journal.recovery));
        } else {
            printf("Issue: interrupted cup update transaction detected in phase '%s'.\n",
                   update_phase_name(update_journal.phase));
        }
        report->issue_count++;
    } else if (journal_kind == RUNTIME_JOURNAL_UNINSTALL) {
        err = uninstall_journal_load(&uninstall_journal, &uninstall_status);
        if (err != CUP_OK || uninstall_status != UNINSTALL_JOURNAL_LOADED) {
            printf("Issue: cup uninstall journal is invalid.\n");
        } else if (uninstall_journal.phase == UNINSTALL_PHASE_FAILED) {
            printf("Issue: the previous cup uninstall failed during '%s' with error %d.\n",
                   uninstall_stage_name(uninstall_journal.stage),
                   uninstall_journal.error_code);
        } else {
            printf("Issue: cup uninstall is pending in phase '%s' during '%s'.\n",
                   uninstall_phase_name(uninstall_journal.phase),
                   uninstall_stage_name(uninstall_journal.stage));
        }
        report->issue_count++;
    }
}

static void check_state_wrappers(const CupState *state,
                                 int state_loaded,
                                 int state_valid,
                                 const PackageCatalog *catalog,
                                 int has_catalog,
                                 DoctorReport *report) {
    WrapperPlan wrappers;
    CupError err;
    size_t wrapper_issues = 0;

    if (!state_loaded) {
        return;
    }

    wrapper_plan_init(&wrappers);
    check_state_packages(state, catalog, has_catalog, report);
    if (state_valid) {
        err = wrapper_plan_build(&wrappers, state);
        if (err == CUP_OK) {
            err = wrapper_plan_check(&wrappers, &wrapper_issues);
        }
        if (err != CUP_OK) {
            report_incomplete(report, "managed wrappers");
        } else {
            report->issue_count += (int)wrapper_issues;
            if (wrapper_issues == 0) {
                printf("OK: managed wrappers are consistent.\n");
            }
        }
    }
    wrapper_plan_free(&wrappers);
}

static void check_package_tree(const CupState *state,
                               int state_loaded,
                               DoctorReport *report) {
    PackageList packages;
    CupError err = package_scan(&packages, NULL);

    if (err == CUP_OK) {
        check_scanned_packages(&packages, state, state_loaded, report);
    } else {
        report_incomplete(report, "installed package tree");
    }
}

static void check_staging_leftovers(DoctorReport *report) {
    CupError err;
    char path[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];
    int staging_exists;
    size_t item_count = 0;

    err = layout_get_staging_dir(path, sizeof(path));
    if (err != CUP_OK) {
        report_incomplete(report, "staging directory");
        return;
    }
    err = layout_get_transaction_path(transaction_path, sizeof(transaction_path));
    if (err != CUP_OK) {
        report_incomplete(report, "transaction journal path");
        return;
    }
    err = system_is_directory(path, &staging_exists);
    if (err != CUP_OK) {
        report_incomplete(report, "staging directory");
        return;
    }
    if (!staging_exists) {
        return;
    }

    err = filesystem_count_children(path, transaction_path, &item_count);
    if (err != CUP_OK) {
        report_incomplete(report, "staging directory contents");
    } else if (item_count > 0) {
        printf("Warning: staging directory contains %zu leftover item(s).\n", item_count);
        report->warning_count++;
    }
}

/* Ordered read-only diagnostic pipeline. */
CupError command_doctor(void) {
    DoctorReport report = {0, 0, 0};
    PackageCatalog catalog;
    CupState state;
    SystemLock lock = {0};
    CupError err;
    char current_host[MAX_PLATFORM_LEN];
    int state_loaded;
    int state_valid;
    int has_catalog = 0;
    int root_snapshot_active = 0;
    size_t root_issue_count = 0;

    package_catalog_init(&catalog);
    printf("==> Checking cup installation...\n");

    err = layout_check_root_candidates(&root_issue_count);
    if (err != CUP_OK) {
        report_incomplete(&report, "cup root candidates");
    } else if (root_issue_count != 0) {
        report.issue_count += (int)root_issue_count;
        printf("Doctor found %d root ownership issue(s). Manual root recovery is required; "
               "'cup repair' cannot select or modify the reported root.\n",
               report.issue_count);
        err = CUP_ERR_INCONSISTENT_STATE;
        goto done;
    }

    /* Doctor must diagnose ambiguous candidates before normal root selection. Once the candidates
     * are valid, freeze the selected root for the remainder of the read-only command. */
    err = layout_root_snapshot_begin();
    if (err != CUP_OK) {
        report_incomplete(&report, "cup root selection");
        err = print_doctor_summary(&report);
        goto done;
    }
    root_snapshot_active = 1;

    /* Installed assets and their catalog belong to the same managed snapshot as state and
     * packages. Only a genuinely missing runtime has no installed state to protect; a failed
     * shared-lock acquisition must not fall back to pathname-based installed-asset reads. */
    {
        DoctorRuntimeSnapshot snapshot = acquire_runtime_snapshot(&report, &lock);

        if (snapshot == DOCTOR_RUNTIME_MISSING) {
            err = check_assets(&catalog, &report, &has_catalog);
            if (err == CUP_OK) {
                err = print_doctor_summary(&report);
            }
            goto done;
        }
        if (snapshot == DOCTOR_RUNTIME_UNAVAILABLE) {
            err = print_doctor_summary(&report);
            goto done;
        }
    }
    err = check_assets(&catalog, &report, &has_catalog);
    if (err != CUP_OK) {
        goto done;
    }

    check_runtime_contents(&report, current_host, sizeof(current_host));
    load_and_check_state(
        &state, current_host, &report, &state_loaded, &state_valid);
    check_transaction_journal(&report);
    check_state_wrappers(
        &state, state_loaded, state_valid, &catalog, has_catalog, &report);
    check_package_tree(&state, state_loaded, &report);
    check_staging_leftovers(&report);
    err = print_doctor_summary(&report);

done:
    package_catalog_free(&catalog);
    system_lock_release(&lock);
    if (root_snapshot_active) {
        layout_root_snapshot_end();
    }
    return err;
}
