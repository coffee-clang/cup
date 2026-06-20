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

#if defined(_WIN32)
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup-windows.ps1"
#else
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup.sh"
#endif

/* Aggregated diagnostic result printed by doctor. */
typedef struct {
    int issues;
    int warnings;
} DoctorReport;

// DIAGNOSTIC HELPERS
static int state_contains_package(const CupState *state, const PackageIdentity *package) {
    char entry[MAX_ENTRY_LEN];

    if (entry_build(entry, sizeof(entry), package->tool, package->version) != CUP_OK) {
        return 0;
    }

    return state_find_installed(state, package->component,
        package->host_platform, package->target_platform, entry) != -1;
}

static int check_required_file(const char *path, const char *description,
    DoctorReport *report) {
    int is_file;

    if (system_is_regular_file(path, &is_file) != CUP_OK || !is_file) {
        printf("Issue: %s is missing or is not a regular file: %s\n",
            description, path);
        report->issues++;
        return 0;
    }

    return 1;
}

static CupError get_installed_bootstrap_presence(int *binary_exists,
    int *manifest_exists, int *uninstall_exists) {
    char path[MAX_PATH_LEN];

    if (binary_exists == NULL || manifest_exists == NULL || uninstall_exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (layout_get_binary_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, binary_exists) != CUP_OK ||
        layout_get_manifest_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, manifest_exists) != CUP_OK ||
        layout_get_uninstall_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, uninstall_exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError check_installed_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    CupError err;
    char path[MAX_PATH_LEN];
    int executable;
    int read_only;
    int exists;

    if (layout_get_binary_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (check_required_file(path, "canonical cup executable", report) &&
        (system_is_executable(path, &executable) != CUP_OK || !executable)) {
        printf("Issue: canonical cup executable is not executable.\n");
        report->issues++;
    }

    if (layout_get_uninstall_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (check_required_file(path, "uninstall script", report)) {
        if (system_is_executable(path, &executable) != CUP_OK || !executable) {
            printf("Issue: uninstall script is not executable.\n");
            report->issues++;
        }

        if (system_is_read_only(path, &read_only) != CUP_OK || !read_only) {
            printf("Issue: uninstall script is not read-only.\n");
            report->issues++;
        }
    }

    if (layout_get_manifest_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (exists && manifest_load_installed(manifest) == CUP_OK) {
        *has_manifest = 1;
        printf("OK: installed package manifest is valid.\n");

        if (system_is_read_only(path, &read_only) != CUP_OK || !read_only) {
            printf("Issue: installed package manifest is not read-only.\n");
            report->issues++;
        }

        return CUP_OK;
    }

    printf("Issue: installed package manifest is %s.\n",
        exists ? "invalid" : "missing");
    report->issues++;

    manifest_free(manifest);
    manifest_init(manifest);
    err = manifest_load_development(manifest);
    if (err == CUP_OK) {
        printf("Info: using the development manifest only for additional diagnostics.\n");
        *has_manifest = 1;
    }

    return CUP_OK;
}

static CupError check_development_bootstrap(Manifest *manifest,
    DoctorReport *report, int *has_manifest) {
    CupError err;
    int executable;

    err = manifest_load_development(manifest);
    if (err != CUP_OK) {
        printf("Issue: development package manifest is missing or invalid.\n");
        report->issues++;
        return CUP_OK;
    }

    *has_manifest = 1;

    if (!check_required_file(DEVELOPMENT_UNINSTALL_PATH,
        "development uninstall script", report)) {
        return CUP_OK;
    }

    if (system_is_executable(DEVELOPMENT_UNINSTALL_PATH, &executable) != CUP_OK ||
        !executable) {
        printf("Issue: development uninstall script is not executable.\n");
        report->issues++;
    }

    printf("OK: development bootstrap files are available.\n");
    return CUP_OK;
}

// DOCTOR COMMAND
CupError handle_doctor(void) {
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
    int binary_exists;
    int manifest_exists;
    int uninstall_exists;
    int installed_bootstrap;
    int state_loaded = 0;
    int has_manifest = 0;
    int lock_exists;
    int tmp_exists;
    size_t missing = 0;
    size_t tmp_count = 0;
    size_t i;

    manifest_init(&manifest);
    transaction_init(&transaction);

    printf("==> Checking cup installation...\n");

    err = get_installed_bootstrap_presence(&binary_exists,
        &manifest_exists, &uninstall_exists);
    if (err != CUP_OK) {
        goto fatal;
    }

    installed_bootstrap = binary_exists || manifest_exists || uninstall_exists;
    if (installed_bootstrap) {
        err = check_installed_bootstrap(&manifest, &report, &has_manifest);
    } else {
        err = check_development_bootstrap(&manifest, &report, &has_manifest);
    }
    if (err != CUP_OK) {
        goto fatal;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        goto fatal;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        printf("Info: cup runtime is not initialized; "
            "the first operational command will create it.\n");
        goto summary;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        printf("Issue: cup runtime structure is incomplete.\n");
        report.issues++;
    }

    if (layout_get_lock_path(path, sizeof(path)) != CUP_OK ||
        system_is_regular_file(path, &lock_exists) != CUP_OK) {
        goto fatal;
    }

    if (!lock_exists) {
        printf("Issue: cup lock file is missing: %s\n", path);
        report.issues++;
    } else {
        err = system_lock_acquire(&lock, path, SYSTEM_LOCK_SHARED);
        if (err != CUP_OK) {
            printf("Issue: %s.\n", err == CUP_ERR_LOCK
                ? "another cup operation is currently running"
                : "cup lock could not be acquired");
            report.issues++;
            goto summary;
        }
    }

    if (layout_check_runtime(&missing) != CUP_OK) {
        goto fatal;
    }
    report.issues += (int)missing;

    err = state_load(&state, &state_status);
    if (err != CUP_OK) {
        printf("Issue: state.txt is syntactically invalid.\n");
        report.issues++;
    } else if (state_status == STATE_FILE_MISSING) {
        printf("Issue: state.txt is missing.\n");
        report.issues++;
    } else {
        state_loaded = 1;

        if (state_validate(&state) != CUP_OK) {
            printf("Issue: state.txt is semantically inconsistent.\n");
            report.issues++;
        } else {
            printf("OK: state.txt is valid.\n");
        }
    }

    err = transaction_load(&transaction, &transaction_status);
    if (err != CUP_OK) {
        printf("Issue: transaction journal is invalid.\n");
        report.issues++;
    } else if (transaction_status == TRANSACTION_FILE_LOADED) {
        printf("Issue: interrupted %s transaction detected for %s@%s.\n",
            transaction_operation_name(transaction.operation),
            transaction.package.tool, transaction.package.version);
        report.issues++;
    }

    if (state_loaded) {
        for (i = 0; i < state.installed_count; ++i) {
            PackageIdentity package;
            int read_only;
            int available;
            char install_path[MAX_PATH_LEN];

            if (package_identity_from_entry(&package, state.installed[i].component,
                state.installed[i].host_platform, state.installed[i].target_platform,
                state.installed[i].entry) != CUP_OK ||
                layout_build_install_path(install_path, sizeof(install_path), &package) != CUP_OK ||
                package_validate(install_path, &package) != CUP_OK) {
                printf("Issue: installed state entry '%s:%s' has no valid package.\n",
                    state.installed[i].component, state.installed[i].entry);
                report.issues++;
                continue;
            }

            if (package_info_is_read_only(install_path, &read_only) != CUP_OK || !read_only) {
                printf("Issue: package metadata for '%s:%s' is not read-only.\n",
                    state.installed[i].component, state.installed[i].entry);
                report.issues++;
            }

            if (has_manifest && manifest_has_version(&manifest, package.component,
                package.tool, package.host_platform, package.target_platform,
                package.version, &available) == CUP_OK && !available) {
                printf("Warning: installed package '%s:%s' is not listed by the active manifest.\n",
                    package.component, state.installed[i].entry);
                report.warnings++;
            }
        }
    }

    err = package_scan(&packages);
    if (err == CUP_OK) {
        if (packages.invalid_count > 0) {
            printf("Issue: components contains %zu invalid or unrecognized package path(s).\n",
                packages.invalid_count);
            report.issues += (int)packages.invalid_count;
        }

        if (state_loaded) {
            for (i = 0; i < packages.count; ++i) {
                if (!state_contains_package(&state, &packages.items[i])) {
                    printf("Issue: valid package '%s@%s' exists in components "
                        "but is absent from state.txt.\n",
                        packages.items[i].tool, packages.items[i].version);
                    report.issues++;
                }
            }
        }
    }

    if (layout_get_tmp_dir(path, sizeof(path)) != CUP_OK ||
        layout_get_transaction_path(transaction_path, sizeof(transaction_path)) != CUP_OK ||
        system_is_directory(path, &tmp_exists) != CUP_OK) {
        goto fatal;
    }

    if (tmp_exists && filesystem_count_children(path, transaction_path, &tmp_count) == CUP_OK &&
        tmp_count > 0) {
        printf("Warning: temporary directory contains %zu leftover item(s).\n", tmp_count);
        report.warnings++;
    }

summary:
    if (report.issues == 0 && report.warnings == 0) {
        printf("Doctor found no issues.\n");
        err = CUP_OK;
    } else if (report.issues == 0) {
        printf("Doctor found %d warning(s), but no blocking issues.\n",
            report.warnings);
        err = CUP_OK;
    } else {
        printf("Doctor found %d issue(s) and %d warning(s). "
            "Run 'cup repair' after reviewing them.\n",
            report.issues, report.warnings);
        err = CUP_ERR_INCONSISTENT_STATE;
    }

    manifest_free(&manifest);
    system_lock_release(&lock);
    return err;

fatal:
    manifest_free(&manifest);
    system_lock_release(&lock);
    return err;
}
