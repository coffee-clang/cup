#include "commands.h"

#include "bootstrap.h"
#include "checksum.h"

#include "entry.h"
#include "fetch.h"
#include "filesystem.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "transaction.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define BOOTSTRAP_URL "https://github.com/coffee-clang/cup/releases/download/cup-bootstrap"

typedef enum {
    REPAIR_ASSET_EXECUTABLE = 1u << 0,
    REPAIR_ASSET_READ_ONLY = 1u << 1
} RepairAssetFlags;


// REPAIR HELPERS
static CupError create_repair_temp(char *path, size_t path_size) {
    char tmp_dir[MAX_PATH_LEN];
    FILE *file = NULL;

    if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        system_create_temp_file(tmp_dir, "repair", path, path_size, &file) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    if (fclose(file) != 0) {
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

static CupError download_asset(const char *asset_name,
    char *path, size_t path_size) {
    char url[MAX_MANIFEST_URL_LEN];

    if (create_repair_temp(path, path_size) != CUP_OK ||
        text_format(url, sizeof(url), "%s/%s", BOOTSTRAP_URL, asset_name) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return fetch_file(url, path, FETCH_VALIDATE_NONEMPTY);
}

static CupError apply_asset_permissions(const char *path,
    RepairAssetFlags flags) {
    if ((flags & REPAIR_ASSET_EXECUTABLE) != 0 &&
        system_set_executable(path, 1) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if ((flags & REPAIR_ASSET_READ_ONLY) != 0 &&
        system_set_read_only(path, 1) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError restore_asset_backup(const char *backup_path,
    const char *destination) {
    SystemCommitState restore_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    err = system_move_path(backup_path, destination, &restore_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    return CUP_ERR_ROLLBACK;
}

static CupError commit_asset(const char *staged_path, const char *destination,
    const char *backup_description) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char backup_path[MAX_PATH_LEN];
    int has_backup = 0;
    int destination_exists;

    err = system_path_exists(destination, &destination_exists);
    if (err != CUP_OK) {
        return err;
    }

    if (destination_exists) {
        err = filesystem_backup_invalid(destination,
            backup_path, sizeof(backup_path));
        if (err != CUP_OK) {
            return err;
        }
        has_backup = 1;
    }

    err = system_replace_file(staged_path, destination, &commit_state);
    if (err == CUP_OK) {
        if (has_backup) {
            printf("Preserved invalid %s as '%s'.\n",
                backup_description, backup_path);
        }
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_APPLIED) {
        if (has_backup) {
            printf("Preserved invalid %s as '%s'.\n",
                backup_description, backup_path);
        }
        return CUP_ERR_COMMIT;
    }

    if (has_backup &&
        restore_asset_backup(backup_path, destination) != CUP_OK) {
        fprintf(stderr,
            "Error: replacement failed and the previous %s could not be restored.\n",
            backup_description);
        return CUP_ERR_ROLLBACK;
    }

    return err;
}

static CupError restore_asset(const char *destination,
    const char *asset_name, const char *checksum_path,
    RepairAssetFlags flags) {
    CupError err;
    char staged_path[MAX_PATH_LEN];
    int matches;

    err = download_asset(asset_name, staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }

    err = bootstrap_verify_asset(checksum_path, asset_name,
        staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        fprintf(stderr, "Error: checksum verification failed for '%s'.\n",
            asset_name);
        return CUP_ERR_VALIDATION;
    }

    err = apply_asset_permissions(staged_path, flags);
    if (err != CUP_OK) {
        system_remove_file(staged_path);
        return err;
    }

    err = commit_asset(staged_path, destination, "file");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }

    return err;
}

static CupError repair_checksum_file(const char *destination,
    const char *asset_name, const char *const *required_assets,
    size_t required_count) {
    CupError err;
    char staged_path[MAX_PATH_LEN];
    int is_regular;
    int is_read_only;

    err = system_is_regular_file(destination, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular && checksum_validate_assets(destination,
        required_assets, required_count) == CUP_OK) {
        err = system_is_read_only(destination, &is_read_only);
        if (err != CUP_OK) {
            return err;
        }
        if (!is_read_only) {
            return system_set_read_only(destination, 1);
        }
        return CUP_OK;
    }

    err = download_asset(asset_name, staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }
    if (checksum_validate_assets(staged_path,
            required_assets, required_count) != CUP_OK ||
        system_set_read_only(staged_path, 1) != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    err = commit_asset(staged_path, destination, "checksum file");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }
    return err;
}

static CupError repair_bootstrap_checksums(void) {
    CupError err;
    char common_path[MAX_PATH_LEN];
    char platform_path[MAX_PATH_LEN];
    char platform_name[MAX_NAME_LEN];
    char binary_asset[MAX_NAME_LEN];
    const char *common_assets[] = {CUP_MANIFEST_FILENAME};
    const char *platform_assets[2];

    if (layout_get_common_checksums_path(common_path,
            sizeof(common_path)) != CUP_OK ||
        layout_get_platform_checksums_path(platform_path,
            sizeof(platform_path)) != CUP_OK ||
        bootstrap_platform_checksums_name(platform_name,
            sizeof(platform_name)) != CUP_OK ||
        bootstrap_binary_asset_name(binary_asset,
            sizeof(binary_asset)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;

    err = repair_checksum_file(common_path, CUP_COMMON_CHECKSUMS_FILENAME,
        common_assets, sizeof(common_assets) / sizeof(common_assets[0]));
    if (err != CUP_OK) {
        return err;
    }
    return repair_checksum_file(platform_path, platform_name,
        platform_assets, sizeof(platform_assets) / sizeof(platform_assets[0]));
}

// OFFICIAL FILES
static CupError repair_manifest(void) {
    Manifest manifest;
    CupError err;
    char manifest_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_regular;
    int matches;
    int is_read_only;

    if (layout_get_manifest_path(manifest_path, sizeof(manifest_path)) != CUP_OK ||
        layout_get_common_checksums_path(checksums_path,
            sizeof(checksums_path)) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = system_is_regular_file(manifest_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular && bootstrap_verify_asset(checksums_path,
            CUP_MANIFEST_FILENAME, manifest_path, &matches) == CUP_OK && matches) {
        manifest_init(&manifest);
        err = manifest_load_installed(&manifest);
        manifest_free(&manifest);
        if (err == CUP_OK) {
            err = system_is_read_only(manifest_path, &is_read_only);
            if (err != CUP_OK) {
                return err;
            }
            if (!is_read_only) {
                printf("Restoring read-only protection on packages.cfg.\n");
                return system_set_read_only(manifest_path, 1);
            }
            return CUP_OK;
        }
    }

    err = download_asset(CUP_MANIFEST_FILENAME,
        staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }
    err = bootstrap_verify_asset(checksums_path, CUP_MANIFEST_FILENAME,
        staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    manifest_init(&manifest);
    err = manifest_load_path(&manifest, staged_path,
        MANIFEST_SOURCE_INSTALLED);
    manifest_free(&manifest);
    if (err != CUP_OK || system_set_read_only(staged_path, 1) != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_MANIFEST;
    }

    err = commit_asset(staged_path, manifest_path, "manifest");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }
    if (err == CUP_OK) {
        printf("Restored official package manifest.\n");
    }
    return err;
}

static CupError repair_binary(void) {
    CupError err;
    char binary_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    char asset_name[MAX_NAME_LEN];
    int is_regular;
    int is_executable;
    int matches;

    if (layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        layout_get_platform_checksums_path(checksums_path,
            sizeof(checksums_path)) != CUP_OK ||
        bootstrap_binary_asset_name(asset_name, sizeof(asset_name)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(binary_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular && bootstrap_verify_asset(checksums_path, asset_name,
            binary_path, &matches) == CUP_OK && matches) {
        err = system_is_executable(binary_path, &is_executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!is_executable) {
            err = system_set_executable(binary_path, 1);
            if (err == CUP_OK) {
                printf("Restored executable permissions on canonical cup executable.\n");
            }
            return err;
        }
        return CUP_OK;
    }

#if defined(_WIN32)
    fprintf(stderr, "Error: the running cup executable is missing or altered. "
        "Run the official installer to replace it safely on Windows.\n");
    return CUP_ERR_VALIDATION;
#else
    printf("Restoring canonical cup executable.\n");
    return restore_asset(binary_path, asset_name, checksums_path,
        REPAIR_ASSET_EXECUTABLE);
#endif
}

static CupError repair_uninstall_script(void) {
    CupError err;
    char script_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    int is_regular;
    int is_read_only;
    int matches;

    if (layout_get_uninstall_path(script_path, sizeof(script_path)) != CUP_OK ||
        layout_get_platform_checksums_path(checksums_path,
            sizeof(checksums_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(script_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular || bootstrap_verify_asset(checksums_path,
            CUP_UNINSTALL_FILENAME, script_path, &matches) != CUP_OK || !matches) {
        printf("Restoring uninstall script.\n");
#if defined(_WIN32)
        return restore_asset(script_path, CUP_UNINSTALL_FILENAME,
            checksums_path, REPAIR_ASSET_READ_ONLY);
#else
        return restore_asset(script_path, CUP_UNINSTALL_FILENAME,
            checksums_path,
            REPAIR_ASSET_EXECUTABLE | REPAIR_ASSET_READ_ONLY);
#endif
    }

#if !defined(_WIN32)
    {
        int is_executable;

        err = system_is_executable(script_path, &is_executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!is_executable) {
            err = system_set_executable(script_path, 1);
            if (err != CUP_OK) {
                return err;
            }
            printf("Restored executable permissions on uninstall script.\n");
        }
    }
#endif

    err = system_is_read_only(script_path, &is_read_only);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_read_only) {
        err = system_set_read_only(script_path, 1);
        if (err == CUP_OK) {
            printf("Restored read-only protection on uninstall script.\n");
        }
    }
    return err;
}

static CupError repair_bootstrap_assets(void) {
    CupError err;

    err = repair_bootstrap_checksums();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_binary();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_uninstall_script();
    if (err != CUP_OK) {
        return err;
    }

    return repair_manifest();
}

// STATE RECONCILIATION
static CupError remove_stale_installed_entries(CupState *state,
    const PackageList *packages, int *state_changed) {
    size_t index = 0;

    while (index < state->installed_count) {
        PackageIdentity package;
        StateEntry entry = state->installed[index];

        if (package_identity_from_entry(&package, entry.component,
            entry.host_platform, entry.target_platform, entry.entry) == CUP_OK &&
            package_list_contains(packages, &package)) {
            index++;
            continue;
        }

        state_clear_matching_default(state, entry.component,
            entry.host_platform, entry.target_platform, entry.entry);
        state_remove_installed(state, entry.component,
            entry.host_platform, entry.target_platform, entry.entry);

        printf("Removed stale state entry '%s:%s'.\n",
            entry.component, entry.entry);
        *state_changed = 1;
    }

    return CUP_OK;
}

static CupError adopt_scanned_packages(CupState *state,
    const PackageList *packages, int *state_changed) {
    size_t i;

    for (i = 0; i < packages->count; ++i) {
        const PackageIdentity *package = &packages->items[i];
        char entry[MAX_ENTRY_LEN];
        char install_path[MAX_PATH_LEN];
        int is_read_only;

        if (entry_build(entry, sizeof(entry),
            package->tool, package->version) != CUP_OK) {
            continue;
        }

        if (state_find_installed(state, package->component,
            package->host_platform, package->target_platform, entry) == -1) {
            CupError err = state_add_installed(state, package->component,
                package->host_platform, package->target_platform, entry);
            if (err != CUP_OK) {
                return err;
            }

            printf("Adopted valid package '%s:%s' into state.txt.\n",
                package->component, entry);
            *state_changed = 1;
        }

        if (layout_build_install_path(install_path,
            sizeof(install_path), package) == CUP_OK &&
            (package_info_is_read_only(install_path, &is_read_only) != CUP_OK ||
                !is_read_only)) {
            if (package_set_info_read_only(install_path) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored read-only protection for %s@%s metadata.\n",
                package->tool, package->version);
        }
    }

    return CUP_OK;
}

static CupError remove_stale_defaults(CupState *state, int *state_changed) {
    size_t index = 0;

    while (index < state->default_count) {
        StateEntry entry = state->defaults[index];

        if (state_find_installed(state, entry.component,
            entry.host_platform, entry.target_platform, entry.entry) != -1) {
            index++;
            continue;
        }

        state_clear_default(state, entry.component,
            entry.host_platform, entry.target_platform);
        printf("Removed stale default for component '%s'.\n", entry.component);
        *state_changed = 1;
    }

    return CUP_OK;
}

static CupError reconcile_state(CupState *state,
    const PackageList *packages, int *state_changed) {
    CupError err;

    err = remove_stale_installed_entries(state, packages, state_changed);
    if (err != CUP_OK) {
        return err;
    }

    err = adopt_scanned_packages(state, packages, state_changed);
    if (err != CUP_OK) {
        return err;
    }

    return remove_stale_defaults(state, state_changed);
}

// REPAIR COMMAND
typedef struct {
    SystemLock lock;
    CupState state;
    StateFileStatus state_status;
    PackageList packages;
    Transaction transaction;
    int state_changed;
    int preserve_tmp;
} RepairContext;

static void repair_context_init(RepairContext *context) {
    memset(context, 0, sizeof(*context));
    transaction_init(&context->transaction);
}

static CupError repair_load_state(RepairContext *context) {
    Transaction pending_transaction;
    TransactionFileStatus transaction_status;
    CupError err;
    char state_path[MAX_PATH_LEN];
    char backup_path[MAX_PATH_LEN];

    err = state_load(&context->state, &context->state_status);
    if (err == CUP_OK &&
        (context->state_status == STATE_FILE_MISSING ||
            state_validate(&context->state) == CUP_OK)) {
        return CUP_OK;
    }

    transaction_init(&pending_transaction);
    err = transaction_load(&pending_transaction, &transaction_status);
    if (err == CUP_OK && transaction_status == TRANSACTION_FILE_LOADED) {
        fprintf(stderr, "Error: state.txt is invalid while a transaction is pending; "
            "automatic recovery would be ambiguous.\n");
        return CUP_ERR_TRANSACTION;
    }

    err = layout_get_state_path(state_path, sizeof(state_path));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    err = filesystem_backup_invalid(state_path,
        backup_path, sizeof(backup_path));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    printf("Preserved invalid state as '%s'.\n", backup_path);
    memset(&context->state, 0, sizeof(context->state));
    context->state_status = STATE_FILE_MISSING;
    context->state_changed = 1;
    return CUP_OK;
}

static CupError repair_pending_transaction(RepairContext *context) {
    TransactionFileStatus transaction_status;
    CupError err;
    char transaction_path[MAX_PATH_LEN];
    char backup_path[MAX_PATH_LEN];

    err = transaction_load(&context->transaction, &transaction_status);
    if (err != CUP_OK) {
        err = layout_get_transaction_path(transaction_path,
            sizeof(transaction_path));
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }

        err = filesystem_backup_invalid(transaction_path,
            backup_path, sizeof(backup_path));
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }

        printf("Preserved invalid transaction journal as '%s'; "
            "ambiguous temporary data was left untouched.\n", backup_path);
        context->preserve_tmp = 1;
        return CUP_OK;
    }

    if (transaction_status == TRANSACTION_FILE_MISSING) {
        return CUP_OK;
    }

    err = transaction_recover(&context->transaction, &context->state);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: interrupted transaction cannot be repaired safely.\n");
    }

    return err;
}

static CupError repair_bootstrap(void) {
    BootstrapInspection inspection;
    CupError err;

    err = bootstrap_inspect(&inspection);
    if (err != CUP_OK) {
        return err;
    }
    if (!bootstrap_has_installed_assets(&inspection) &&
        bootstrap_development_is_valid(&inspection)) {
        printf("Using development bootstrap files from the repository.\n");
        return CUP_OK;
    }

    err = layout_ensure_bootstrap();
    if (err != CUP_OK) {
        return err;
    }
    return repair_bootstrap_assets();
}

static CupError quarantine_invalid_packages(PackageList *packages,
    int *quarantined_any) {
    size_t i;

    *quarantined_any = 0;
    for (i = 0; i < packages->issue_count; ++i) {
        const PackageIssue *issue = &packages->issues[i];

        if (issue->can_quarantine) {
            CupError err;
            char recovery_path[MAX_PATH_LEN];

            err = package_quarantine(issue,
                recovery_path, sizeof(recovery_path));
            if (err != CUP_OK) {
                return err;
            }

            printf("Quarantined invalid package '%s' as '%s'.\n",
                issue->path, recovery_path);
            *quarantined_any = 1;
            continue;
        }

        printf("Warning: package path '%s' was left unchanged: %s.\n",
            issue->path, package_issue_reason_name(issue->reason));
    }

    return CUP_OK;
}

static CupError require_complete_package_scan(const PackageList *packages) {
    if (packages->complete) {
        return CUP_OK;
    }

    fprintf(stderr,
        "Error: package scan exceeded its in-memory capacity; "
        "repair did not modify package state.\n");
    return CUP_ERR_INCONSISTENT_STATE;
}

static CupError require_representable_package_scan(
    const PackageList *packages) {
    if (packages->total_count <= MAX_INSTALLED) {
        return CUP_OK;
    }

    fprintf(stderr,
        "Error: %zu valid packages were found, but state.txt supports at most "
        "%d installed entries; repair did not modify packages or state.\n",
        packages->total_count, MAX_INSTALLED);
    return CUP_ERR_INCONSISTENT_STATE;
}

static CupError repair_packages(RepairContext *context) {
    CupError err;
    int quarantined_any;

    err = package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    err = require_complete_package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    err = require_representable_package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    err = quarantine_invalid_packages(&context->packages,
        &quarantined_any);
    if (err != CUP_OK) {
        return err;
    }

    if (quarantined_any) {
        err = package_scan(&context->packages);
        if (err != CUP_OK) {
            return err;
        }

        err = require_complete_package_scan(&context->packages);
        if (err != CUP_OK) {
            return err;
        }

        err = require_representable_package_scan(&context->packages);
        if (err != CUP_OK) {
            return err;
        }
    }

    return reconcile_state(&context->state, &context->packages,
        &context->state_changed);
}

static CupError repair_save_state(const RepairContext *context) {
    CupError err;

    if (context->state_status == STATE_FILE_LOADED &&
        !context->state_changed) {
        return CUP_OK;
    }

    err = state_save(&context->state);
    if (err != CUP_OK) {
        return err;
    }

    printf("Saved a valid state.txt.\n");
    return CUP_OK;
}

static CupError reject_pending_uninstall(void) {
    CupError err;
    int pending;

    err = bootstrap_uninstall_is_pending(&pending);
    if (err != CUP_OK) {
        return err;
    }
    if (!pending) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: cup uninstall is in progress or did not finish. "
        "Run the installer again if the marker is stale.\n");
    return CUP_ERR_LOCK;
}

static CupError repair_cleanup_tmp(const RepairContext *context) {
    char tmp_dir[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];

    if (context->preserve_tmp) {
        return CUP_OK;
    }

    if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        layout_get_transaction_path(transaction_path,
            sizeof(transaction_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return filesystem_clear_directory(tmp_dir, transaction_path);
}

CupError command_repair(void) {
    RepairContext context;
    CupError err;
    char lock_path[MAX_PATH_LEN];

    repair_context_init(&context);
    printf("==> Repairing cup...\n");

    err = layout_ensure_root();
    if (err != CUP_OK) {
        return err;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_lock_acquire(&context.lock,
        lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr,
                "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    err = reject_pending_uninstall();
    if (err == CUP_OK) {
        err = layout_ensure_runtime();
    }
    if (err == CUP_OK) {
        err = repair_load_state(&context);
    }
    if (err == CUP_OK) {
        err = repair_pending_transaction(&context);
    }
    if (err == CUP_OK) {
        err = repair_bootstrap();
    }
    if (err == CUP_OK) {
        err = repair_packages(&context);
    }
    if (err == CUP_OK) {
        err = repair_save_state(&context);
    }
    if (err == CUP_OK) {
        err = repair_cleanup_tmp(&context);
    }

    if (err == CUP_OK) {
        printf("Repair completed.\n");
    }

    system_lock_release(&context.lock);
    return err;
}
