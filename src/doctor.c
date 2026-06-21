#include "commands.h"

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
} DoctorReport;

typedef struct {
    int binary_exists;
    int manifest_exists;
    int uninstall_exists;
} BootstrapPresence;

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

static int check_regular_file(const char *path, const char *description,
    DoctorReport *report) {
    int is_regular_file;

    if (system_is_regular_file(path, &is_regular_file) != CUP_OK ||
        !is_regular_file) {
        printf("Issue: %s is missing or is not a regular file: %s\n",
            description, path);
        report->issue_count++;
        return 0;
    }

    return 1;
}

static void check_executable_file(const char *path, const char *description,
    DoctorReport *report) {
    int is_executable;

    if (system_is_executable(path, &is_executable) != CUP_OK ||
        !is_executable) {
        printf("Issue: %s is not executable.\n", description);
        report->issue_count++;
    }
}

static void check_read_only_file(const char *path, const char *description,
    DoctorReport *report) {
    int is_read_only;

    if (system_is_read_only(path, &is_read_only) != CUP_OK ||
        !is_read_only) {
        printf("Issue: %s is not read-only.\n", description);
        report->issue_count++;
    }
}

static CupError inspect_installed_bootstrap(BootstrapPresence *presence) {
    char path[MAX_PATH_LEN];

    if (presence == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (layout_get_binary_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &presence->binary_exists) != CUP_OK ||
        layout_get_manifest_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &presence->manifest_exists) != CUP_OK ||
        layout_get_uninstall_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &presence->uninstall_exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static int bootstrap_is_installed(const BootstrapPresence *presence) {
    return presence->binary_exists ||
        presence->manifest_exists ||
        presence->uninstall_exists;
}

static CupError check_installed_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    CupError err;
    char path[MAX_PATH_LEN];
    int manifest_exists;

    err = layout_get_binary_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    if (check_regular_file(path, "canonical cup executable", report)) {
        check_executable_file(path, "canonical cup executable", report);
    }

    err = layout_get_uninstall_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    if (check_regular_file(path, "uninstall script", report)) {
#if !defined(_WIN32)
        check_executable_file(path, "uninstall script", report);
#endif
        check_read_only_file(path, "uninstall script", report);
    }

    err = layout_get_manifest_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_path_exists(path, &manifest_exists);
    if (err != CUP_OK) {
        return err;
    }

    if (manifest_exists && manifest_load_installed(manifest) == CUP_OK) {
        *has_manifest = 1;
        printf("OK: installed package manifest is valid.\n");
        check_read_only_file(path, "installed package manifest", report);
        return CUP_OK;
    }

    printf("Issue: installed package manifest is %s.\n",
        manifest_exists ? "invalid" : "missing");
    report->issue_count++;

    manifest_free(manifest);
    manifest_init(manifest);
    err = manifest_load_development(manifest);
    if (err == CUP_OK) {
        printf("Info: using the development manifest only for "
            "additional diagnostics.\n");
        *has_manifest = 1;
    }

    return CUP_OK;
}

static CupError check_development_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    CupError err;

    err = manifest_load_development(manifest);
    if (err != CUP_OK) {
        printf("Issue: development package manifest is missing or invalid.\n");
        report->issue_count++;
        return CUP_OK;
    }

    *has_manifest = 1;
    if (!check_regular_file(CUP_DEVELOPMENT_UNINSTALL_PATH,
        "development uninstall script", report)) {
        return CUP_OK;
    }

#if !defined(_WIN32)
    check_executable_file(CUP_DEVELOPMENT_UNINSTALL_PATH,
        "development uninstall script", report);
#endif

    printf("OK: development bootstrap files are available.\n");
    return CUP_OK;
}

static CupError check_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    BootstrapPresence presence;
    CupError err;

    err = inspect_installed_bootstrap(&presence);
    if (err != CUP_OK) {
        return err;
    }

    if (bootstrap_is_installed(&presence)) {
        return check_installed_bootstrap(manifest, report, has_manifest);
    }

    return check_development_bootstrap(manifest, report, has_manifest);
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

        if (package_info_is_read_only(install_path, &is_read_only) != CUP_OK ||
            !is_read_only) {
            printf("Issue: package metadata for '%s:%s' is not read-only.\n",
                state_entry->component, state_entry->entry);
            report->issue_count++;
        }

        if (has_manifest && manifest_has_version(manifest, package.component,
            package.tool, package.host_platform, package.target_platform,
            package.version, &is_available) == CUP_OK && !is_available) {
            printf("Warning: installed package '%s:%s' is not listed "
                "by the active manifest.\n",
                package.component, state_entry->entry);
            report->warning_count++;
        }
    }
}

static void check_scanned_packages(const PackageList *packages,
    const CupState *state, int state_loaded, DoctorReport *report) {
    size_t i;

    if (packages->invalid_count > 0) {
        printf("Issue: components contains %zu invalid or unrecognized "
            "package path(s).\n", packages->invalid_count);
        report->issue_count += (int)packages->invalid_count;
    }

    if (!state_loaded) {
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
    DoctorReport report = {0, 0};
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

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
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
        goto done;
    }

    err = system_is_regular_file(path, &lock_exists);
    if (err != CUP_OK) {
        goto done;
    }

    if (!lock_exists) {
        printf("Issue: cup lock file is missing: %s\n", path);
        report.issue_count++;
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
        goto done;
    }
    report.issue_count += (int)missing_count;

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
    }

    err = layout_get_tmp_dir(path, sizeof(path));
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_get_transaction_path(transaction_path,
        sizeof(transaction_path));
    if (err != CUP_OK) {
        goto done;
    }

    err = system_is_directory(path, &tmp_exists);
    if (err != CUP_OK) {
        goto done;
    }

    if (tmp_exists &&
        filesystem_count_children(path, transaction_path, &tmp_count) == CUP_OK &&
        tmp_count > 0) {
        printf("Warning: temporary directory contains %zu leftover item(s).\n",
            tmp_count);
        report.warning_count++;
    }

    err = print_doctor_summary(&report);

done:
    manifest_free(&manifest);
    system_lock_release(&lock);
    return err;
}
