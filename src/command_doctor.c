/*
 * Performs read-only diagnosis of CUP assets, runtime layout, state, packages, transactions and
 * managed wrappers. Failed inspections are reported as incomplete rather than silently ignored.
 */

#include "commands.h"

#include "cup_assets.h"
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
#include "cup_update_journal.h"
#include "runtime_journal.h"

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
                                CupAssetStatus status) {
    if (status == CUP_ASSET_VALID) {
        printf("OK: %s is valid.\n", description);
        return;
    }

    printf("Issue: %s is %s.\n", description, status == CUP_ASSET_MISSING ? "missing" : "invalid");
    report->issue_count++;
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

static void check_cup_update_result(DoctorReport *report) {
    CupUpdateResult result;
    CupError err;

    err = cup_update_result_load(&result);
    if (err != CUP_OK) {
        printf("Issue: the previous CUP update result is invalid.\n");
        report->issue_count++;
        return;
    }
    if (result.status == CUP_UPDATE_RESULT_FAILED) {
        printf("Issue: the previous CUP update failed with error %d at version %s.\n",
               result.error_code,
               result.version);
        report->issue_count++;
    } else if (result.status == CUP_UPDATE_RESULT_SUCCESS) {
        printf("OK: the previous CUP update completed successfully at version %s.\n",
               result.version);
    }
}

static CupError load_diagnostic_catalog(const CupAssetsInspection *inspection,
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

static CupError check_cup_assets(PackageCatalog *catalog, DoctorReport *report, int *has_catalog) {
    CupAssetsInspection inspection;
    CupError err;
    char path[MAX_PATH_LEN];

    err = cup_assets_inspect(&inspection);
    if (err != CUP_OK) {
        report_incomplete(report, "CUP assets");
        return CUP_OK;
    }

    if (cup_assets_has_installed_assets(&inspection)) {
        report_asset_status(report, "canonical cup executable", inspection.binary);
        report_asset_status(report, "native CUP update helper", inspection.helper);
        report_asset_status(report, "installed package catalog", inspection.catalog);
        report_asset_status(report, "installation configuration", inspection.install_policy);
        report_asset_status(report, "uninstall script", inspection.uninstall);
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
        if (inspection.uninstall == CUP_ASSET_VALID &&
            layout_get_uninstall_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "uninstall script", report);
        }
        if (inspection.common_checksums == CUP_ASSET_VALID &&
            layout_get_common_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "common checksum file", report);
        }
        if (inspection.platform_checksums == CUP_ASSET_VALID &&
            layout_get_platform_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "platform checksum file", report);
        }
    } else if (cup_assets_development_is_valid(&inspection)) {
        printf("OK: development CUP assets are available.\n");
    } else {
        printf("Issue: neither installed nor development CUP assets "
               "are complete and valid.\n");
        report->issue_count++;
    }

    err = load_diagnostic_catalog(&inspection, catalog, has_catalog);
    if (err != CUP_OK) {
        report_incomplete(report, "active package catalog");
    }
    return CUP_OK;
}

static void check_state_packages(const CupState *state,
                                 const PackageCatalog *catalog,
                                 int has_catalog,
                                 DoctorReport *report) {
    size_t i;

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

        err = package_validate(install_path, package);
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
                       "by the active catalog.\n",
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

/* Ordered read-only diagnostic pipeline. */
CupError command_doctor(void) {
    DoctorReport report = {0, 0, 0};
    LayoutRuntimeStatus runtime_status;
    CupState state;
    StateFileStatus state_status = STATE_FILE_MISSING;
    PackageCatalog catalog;
    PackageList packages;
    PackageTransaction package_transaction;
    PackageTransactionStatus package_status;
    CupUpdateJournal cup_update_journal;
    CupUpdateJournalStatus cup_update_status;
    RuntimeJournalKind journal_kind;
    SystemLock lock = {0, 0};
    CupError err;
    char path[MAX_PATH_LEN];
    char current_host[MAX_PLATFORM_LEN];
    char transaction_path[MAX_PATH_LEN];
    int state_loaded = 0;
    int state_valid = 0;
    int has_catalog = 0;
    int lock_exists;
    int tmp_exists;
    size_t missing_count = 0;
    size_t tmp_count = 0;

    package_catalog_init(&catalog);
    package_transaction_init(&package_transaction);
    cup_update_journal_init(&cup_update_journal);
    printf("==> Checking cup installation...\n");

    err = check_cup_assets(&catalog, &report, &has_catalog);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_get_uninstall_marker_path(path, sizeof(path));
    if (err != CUP_OK) {
        report_incomplete(&report, "uninstall marker");
    } else {
        SystemPathKind marker_kind;

        err = system_get_path_kind(path, &marker_kind);
        if (err != CUP_OK) {
            report_incomplete(&report, "uninstall marker");
        } else if (marker_kind != SYSTEM_PATH_MISSING) {
            printf("Issue: an uninstall marker exists: %s\n", path);
            report.issue_count++;
        }
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        report_incomplete(&report, "runtime structure");
        err = print_doctor_summary(&report);
        goto done;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        printf("Info: cup runtime is not initialized; "
               "the first operational command will create it.\n");
        err = print_doctor_summary(&report);
        goto done;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        printf("Issue: cup runtime structure is incomplete.\n");
        report.issue_count++;
    }

    err = layout_get_lock_path(path, sizeof(path));
    if (err != CUP_OK) {
        report_incomplete(&report, "lock file");
        err = print_doctor_summary(&report);
        goto done;
    }

    err = system_is_regular_file(path, &lock_exists);
    if (err != CUP_OK) {
        report_incomplete(&report, "lock file");
        err = print_doctor_summary(&report);
        goto done;
    }

    if (!lock_exists) {
        printf("Issue: cup lock file is missing: %s\n", path);
        report.issue_count++;
        report_incomplete(&report, "coherent runtime snapshot");
        err = print_doctor_summary(&report);
        goto done;
    } else {
        err = system_lock_acquire(&lock, path, SYSTEM_LOCK_SHARED);
        if (err != CUP_OK) {
            printf("Issue: %s.\n",
                   err == CUP_ERR_LOCK ? "another cup operation is currently running"
                                       : "cup lock could not be acquired");
            report.issue_count++;
            err = print_doctor_summary(&report);
            goto done;
        }
    }

    err = platform_get_host(current_host, sizeof(current_host));
    if (err != CUP_OK) {
        report_incomplete(&report, "current host platform");
        current_host[0] = '\0';
    }

    err = layout_check_runtime(&missing_count);
    if (err != CUP_OK) {
        report_incomplete(&report, "runtime contents");
    } else {
        report.issue_count += (int)missing_count;
    }

    err = state_load(&state, &state_status);
    if (err != CUP_OK) {
        printf("Issue: state.txt is syntactically invalid.\n");
        report.issue_count++;
    } else if (state_status == STATE_FILE_MISSING) {
        printf("Issue: state.txt is missing.\n");
        report.issue_count++;
    } else {
        state_loaded = 1;
        if (state_validate(&state) != CUP_OK) {
            printf("Issue: state.txt is semantically inconsistent.\n");
            report.issue_count++;
        } else {
            size_t foreign_records;

            state_valid = 1;
            foreign_records = state_count_foreign_hosts(&state, current_host);
            printf("OK: state.txt is structurally valid.\n");
            if (foreign_records > 0) {
                printf("Warning: state.txt preserves %zu record(s) for foreign hosts; operational "
                       "commands will not manage them.\n",
                       foreign_records);
                report.warning_count++;
            }
        }
    }

    err = runtime_journal_detect(&journal_kind);
    if (err != CUP_OK) {
        printf("Issue: transaction journal is invalid.\n");
        report.issue_count++;
    } else if (journal_kind == RUNTIME_JOURNAL_PACKAGE) {
        err = package_transaction_load(&package_transaction, &package_status);
        if (err != CUP_OK || package_status != PACKAGE_TRANSACTION_LOADED) {
            printf("Issue: package transaction journal is invalid.\n");
        } else {
            printf("Issue: interrupted %s transaction detected for %s@%s.\n",
                   package_operation_name(package_transaction.operation),
                   package_transaction.package.tool,
                   package_transaction.package.version);
        }
        report.issue_count++;
    } else if (journal_kind == RUNTIME_JOURNAL_CUP_UPDATE) {
        err = cup_update_journal_load(&cup_update_journal, &cup_update_status);
        printf(err == CUP_OK && cup_update_status == CUP_UPDATE_JOURNAL_LOADED
                   ? "Issue: interrupted CUP update transaction detected.\n"
                   : "Issue: CUP update journal is invalid.\n");
        report.issue_count++;
    }

    check_cup_update_result(&report);

    if (state_loaded) {
        WrapperPlan wrappers;
        size_t wrapper_issues = 0;

        wrapper_plan_init(&wrappers);
        check_state_packages(&state, &catalog, has_catalog, &report);
        if (state_valid) {
            err = wrapper_plan_build(&wrappers, &state);
            if (err == CUP_OK) {
                err = wrapper_plan_check(&wrappers, &wrapper_issues);
            }
            if (err != CUP_OK) {
                report_incomplete(&report, "managed wrappers");
            } else {
                report.issue_count += (int)wrapper_issues;
                if (wrapper_issues == 0) {
                    printf("OK: managed wrappers are consistent.\n");
                }
            }
        }
        wrapper_plan_free(&wrappers);
    }

    err = package_scan(&packages);
    if (err == CUP_OK) {
        check_scanned_packages(&packages, &state, state_loaded, &report);
    } else {
        report_incomplete(&report, "installed package tree");
    }

    err = layout_get_staging_dir(path, sizeof(path));
    if (err != CUP_OK) {
        report_incomplete(&report, "staging directory");
    } else if (layout_get_transaction_path(transaction_path, sizeof(transaction_path)) != CUP_OK) {
        report_incomplete(&report, "transaction journal path");
    } else if (system_is_directory(path, &tmp_exists) != CUP_OK) {
        report_incomplete(&report, "staging directory");
    } else if (tmp_exists) {
        err = filesystem_count_children(path, transaction_path, &tmp_count);
        if (err != CUP_OK) {
            report_incomplete(&report, "staging directory contents");
        } else if (tmp_count > 0) {
            printf("Warning: staging directory contains %zu leftover item(s).\n", tmp_count);
            report.warning_count++;
        }
    }

    err = print_doctor_summary(&report);

done:
    package_catalog_free(&catalog);
    system_lock_release(&lock);
    return err;
}
