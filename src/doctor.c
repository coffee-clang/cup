#include "commands.h"

#include "bootstrap.h"
#include "entry.h"
#include "filesystem.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "transaction.h"

#include <stdio.h>

typedef struct {
    int issue_count;
    int warning_count;
    int incomplete_count;
} DoctorReport;

static int state_contains_package(const CupState *state,
    const PackageIdentity *package) {
    char entry[MAX_ENTRY_LEN];

    if (entry_build(entry, sizeof(entry),
        package->tool, package->version) != CUP_OK) {
        return 0;
    }

    return state_find_installed(state, package->component,
        package->host_platform, package->target_platform, entry) != -1;
}

static void report_incomplete(DoctorReport *report,
    const char *description) {
    printf("Incomplete: %s could not be checked.\n", description);
    report->incomplete_count++;
}

static void report_asset_status(DoctorReport *report,
    const char *description, BootstrapAssetStatus status) {
    if (status == BOOTSTRAP_ASSET_VALID) {
        printf("OK: %s is valid.\n", description);
        return;
    }

    printf("Issue: %s is %s.\n", description,
        status == BOOTSTRAP_ASSET_MISSING ? "missing" : "invalid");
    report->issue_count++;
}

static void check_read_only_path(const char *path, const char *description,
    DoctorReport *report) {
    int is_read_only;

    if (system_is_read_only(path, &is_read_only) != CUP_OK) {
        report_incomplete(report, description);
    } else if (!is_read_only) {
        printf("Issue: %s is not read-only.\n", description);
        report->issue_count++;
    }
}

static CupError load_diagnostic_manifest(const BootstrapInspection *inspection,
    Manifest *manifest, int *has_manifest) {
    CupError err;

    *has_manifest = 0;
    if (inspection->manifest == BOOTSTRAP_ASSET_VALID) {
        err = manifest_load_installed(manifest);
    } else if (inspection->development_manifest_valid) {
        err = manifest_load_development(manifest);
        if (err == CUP_OK) {
            printf("Info: using the development manifest only for "
                "additional diagnostics.\n");
        }
    } else {
        return CUP_OK;
    }

    if (err == CUP_OK) {
        *has_manifest = 1;
    }
    return err;
}

static CupError check_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    BootstrapInspection inspection;
    CupError err;
    char path[MAX_PATH_LEN];

    err = bootstrap_inspect(&inspection);
    if (err != CUP_OK) {
        report_incomplete(report, "bootstrap files");
        return CUP_OK;
    }

    if (bootstrap_has_installed_assets(&inspection)) {
        report_asset_status(report, "canonical cup executable",
            inspection.binary);
        report_asset_status(report, "installed package manifest",
            inspection.manifest);
        report_asset_status(report, "uninstall script",
            inspection.uninstall);
        report_asset_status(report, "common checksum file",
            inspection.common_checksums);
        report_asset_status(report, "platform checksum file",
            inspection.platform_checksums);

        if (inspection.manifest == BOOTSTRAP_ASSET_VALID &&
            layout_get_manifest_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "installed package manifest", report);
        }
        if (inspection.uninstall == BOOTSTRAP_ASSET_VALID &&
            layout_get_uninstall_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "uninstall script", report);
        }
        if (inspection.common_checksums == BOOTSTRAP_ASSET_VALID &&
            layout_get_common_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "common checksum file", report);
        }
        if (inspection.platform_checksums == BOOTSTRAP_ASSET_VALID &&
            layout_get_platform_checksums_path(path, sizeof(path)) == CUP_OK) {
            check_read_only_path(path, "platform checksum file", report);
        }
    } else if (bootstrap_development_is_valid(&inspection)) {
        printf("OK: development bootstrap files are available.\n");
    } else {
        printf("Issue: neither installed nor development bootstrap files "
            "are complete and valid.\n");
        report->issue_count++;
    }

    err = load_diagnostic_manifest(&inspection, manifest, has_manifest);
    if (err != CUP_OK) {
        report_incomplete(report, "active package manifest");
    }
    return CUP_OK;
}

static void check_state_packages(const CupState *state,
    const Manifest *manifest, int has_manifest, DoctorReport *report) {
    size_t i;

    for (i = 0; i < state->installed_count; ++i) {
        const StateEntry *state_entry = &state->installed[i];
        PackageIdentity package;
        char install_path[MAX_PATH_LEN];
        int is_read_only;
        int is_available;

        if (package_identity_from_entry(&package, state_entry->component,
            state_entry->host_platform, state_entry->target_platform,
            state_entry->entry) != CUP_OK ||
            layout_build_install_path(install_path,
                sizeof(install_path), &package) != CUP_OK ||
            package_validate(install_path, &package) != CUP_OK) {
            printf("Issue: installed state entry '%s:%s' "
                "has no valid package.\n",
                state_entry->component, state_entry->entry);
            report->issue_count++;
            continue;
        }

        {
            CupError err = package_info_is_read_only(install_path,
                &is_read_only);

            if (err != CUP_OK) {
                printf("Incomplete: package metadata protection for '%s:%s' "
                    "could not be checked.\n",
                    state_entry->component, state_entry->entry);
                report->incomplete_count++;
            } else if (!is_read_only) {
                printf("Issue: package metadata for '%s:%s' is not read-only.\n",
                    state_entry->component, state_entry->entry);
                report->issue_count++;
            }
        }

        if (has_manifest) {
            CupError err = manifest_has_version(manifest, package.component,
                package.tool, package.host_platform, package.target_platform,
                package.version, &is_available);

            if (err != CUP_OK) {
                printf("Incomplete: manifest availability for '%s:%s' "
                    "could not be checked.\n",
                    package.component, state_entry->entry);
                report->incomplete_count++;
            } else if (!is_available) {
                printf("Warning: installed package '%s:%s' is not listed "
                    "by the active manifest.\n",
                    package.component, state_entry->entry);
                report->warning_count++;
            }
        }
    }
}

static void check_scanned_packages(const PackageList *packages,
    const CupState *state, int state_loaded, DoctorReport *report) {
    size_t i;

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

        printf("Issue: %zu additional invalid package path(s) could not be listed.\n",
            omitted);
        report->issue_count++;
    }

    if (!state_loaded || !packages->complete) {
        return;
    }

    for (i = 0; i < packages->count; ++i) {
        if (!state_contains_package(state, &packages->items[i])) {
            printf("Issue: valid package '%s@%s' exists in components "
                "but is absent from state.txt.\n",
                packages->items[i].tool, packages->items[i].version);
            report->issue_count++;
        }
    }
}

static CupError print_doctor_summary(const DoctorReport *report) {
    if (report->incomplete_count > 0) {
        printf("Doctor found %d issue(s), %d warning(s), and %d "
            "incomplete check(s).\n",
            report->issue_count, report->warning_count,
            report->incomplete_count);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (report->issue_count == 0 && report->warning_count == 0) {
        printf("Doctor found no issues.\n");
        return CUP_OK;
    }
    if (report->issue_count == 0) {
        printf("Doctor found %d warning(s), but no blocking issues.\n",
            report->warning_count);
        return CUP_OK;
    }

    printf("Doctor found %d issue(s) and %d warning(s). "
        "Run 'cup repair' after reviewing them.\n",
        report->issue_count, report->warning_count);
    return CUP_ERR_INCONSISTENT_STATE;
}

CupError command_doctor(void) {
    DoctorReport report = {0, 0, 0};
    LayoutRuntimeStatus runtime_status;
    CupState state;
    StateFileStatus state_status = STATE_FILE_MISSING;
    Manifest manifest;
    PackageList packages;
    Transaction transaction;
    TransactionFileStatus transaction_status;
    SystemLock lock = {0, 0};
    CupError err;
    char path[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];
    int state_loaded = 0;
    int has_manifest = 0;
    int lock_exists;
    int tmp_exists;
    size_t missing_count = 0;
    size_t tmp_count = 0;

    manifest_init(&manifest);
    transaction_init(&transaction);
    printf("==> Checking cup installation...\n");

    err = check_bootstrap(&manifest, &report, &has_manifest);
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
            printf("Issue: %s.\n", err == CUP_ERR_LOCK
                ? "another cup operation is currently running"
                : "cup lock could not be acquired");
            report.issue_count++;
            err = print_doctor_summary(&report);
            goto done;
        }
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
            printf("OK: state.txt is valid.\n");
        }
    }

    err = transaction_load(&transaction, &transaction_status);
    if (err != CUP_OK) {
        printf("Issue: transaction journal is invalid.\n");
        report.issue_count++;
    } else if (transaction_status == TRANSACTION_FILE_LOADED) {
        printf("Issue: interrupted %s transaction detected for %s@%s.\n",
            transaction_operation_name(transaction.operation),
            transaction.package.tool, transaction.package.version);
        report.issue_count++;
    }

    if (state_loaded) {
        check_state_packages(&state, &manifest, has_manifest, &report);
    }

    err = package_scan(&packages);
    if (err == CUP_OK) {
        check_scanned_packages(&packages, &state, state_loaded, &report);
    } else {
        report_incomplete(&report, "installed package tree");
    }

    err = layout_get_tmp_dir(path, sizeof(path));
    if (err != CUP_OK) {
        report_incomplete(&report, "temporary directory");
    } else if (layout_get_transaction_path(transaction_path,
            sizeof(transaction_path)) != CUP_OK) {
        report_incomplete(&report, "transaction journal path");
    } else if (system_is_directory(path, &tmp_exists) != CUP_OK) {
        report_incomplete(&report, "temporary directory");
    } else if (tmp_exists) {
        err = filesystem_count_children(path, transaction_path, &tmp_count);
        if (err != CUP_OK) {
            report_incomplete(&report, "temporary directory contents");
        } else if (tmp_count > 0) {
            printf("Warning: temporary directory contains %zu leftover item(s).\n",
                tmp_count);
            report.warning_count++;
        }
    }

    err = print_doctor_summary(&report);

done:
    manifest_free(&manifest);
    system_lock_release(&lock);
    return err;
}
