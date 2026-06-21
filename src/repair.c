#include "commands.h"

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
static CupError installed_bootstrap_exists(int *exists) {
    char paths[3][MAX_PATH_LEN];
    size_t i;
    int path_exists;

    if (exists == NULL ||
        layout_get_binary_path(paths[0], sizeof(paths[0])) != CUP_OK ||
        layout_get_manifest_path(paths[1], sizeof(paths[1])) != CUP_OK ||
        layout_get_uninstall_path(paths[2], sizeof(paths[2])) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    *exists = 0;
    for (i = 0; i < 3; ++i) {
        if (system_path_exists(paths[i], &path_exists) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }

        if (path_exists) {
            *exists = 1;
            break;
        }
    }

    return CUP_OK;
}

static CupError development_bootstrap_available(int *available) {
    Manifest manifest;
    CupError err;
    int uninstall_exists;

    if (available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *available = 0;
    manifest_init(&manifest);
    err = manifest_load_development(&manifest);
    manifest_free(&manifest);
    if (err != CUP_OK) {
        return CUP_OK;
    }

    err = system_is_regular_file(CUP_DEVELOPMENT_UNINSTALL_PATH, &uninstall_exists);
    if (err != CUP_OK) {
        return err;
    }

    *available = uninstall_exists;
    return CUP_OK;
}

static int package_list_contains(const PackageList *packages, const PackageIdentity *package) {
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

    return fetch_resource(url, path);
}

static CupError stage_asset(const char *local_source, const char *asset_name,
    char *staged_path, size_t staged_size) {
    CupError err;
    int is_regular_file;

    if (!text_is_empty(local_source) &&
        system_is_regular_file(local_source, &is_regular_file) == CUP_OK &&
        is_regular_file) {
        err = create_repair_temp(staged_path, staged_size);
        if (err == CUP_OK) {
            err = system_copy_file(local_source, staged_path);
        }
        return err;
    }

    return download_asset(asset_name, staged_path, staged_size);
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

static CupError commit_asset(const char *staged_path, const char *destination,
    const char *backup_description) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char backup_path[MAX_PATH_LEN];
    int destination_exists;

    err = system_path_exists(destination, &destination_exists);
    if (err != CUP_OK) {
        return err;
    }

    if (destination_exists) {
        system_set_read_only(destination, 0);
        err = filesystem_backup_invalid(destination,
            backup_path, sizeof(backup_path));
        if (err != CUP_OK) {
            return err;
        }

        printf("Preserved invalid %s as '%s'.\n",
            backup_description, backup_path);
    }

    err = system_replace_file(staged_path, destination, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

static CupError restore_asset(const char *destination,
    const char *local_source, const char *asset_name, RepairAssetFlags flags) {
    CupError err;
    char staged_path[MAX_PATH_LEN];

    err = stage_asset(local_source, asset_name,
        staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
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

// OFFICIAL FILES
static CupError repair_manifest(void) {
    Manifest manifest;
    CupError err;
    char manifest_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int manifest_exists;
    int is_read_only;

    manifest_init(&manifest);

    err = layout_get_manifest_path(manifest_path, sizeof(manifest_path));
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = system_path_exists(manifest_path, &manifest_exists);
    if (err != CUP_OK) {
        return err;
    }

    if (manifest_exists && manifest_load_installed(&manifest) == CUP_OK) {
        manifest_free(&manifest);

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

    manifest_free(&manifest);
    manifest_init(&manifest);
    err = manifest_load_development(&manifest);
    if (err == CUP_OK) {
        err = create_repair_temp(staged_path, sizeof(staged_path));
        if (err == CUP_OK) {
            err = system_copy_file(manifest.path, staged_path);
        }
        manifest_free(&manifest);
    } else {
        manifest_free(&manifest);
        err = download_asset(CUP_MANIFEST_FILENAME,
            staged_path, sizeof(staged_path));
    }
    if (err != CUP_OK) {
        return err;
    }

    manifest_init(&manifest);
    err = manifest_load_path(&manifest, staged_path,
        MANIFEST_SOURCE_INSTALLED);
    manifest_free(&manifest);
    if (err != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_MANIFEST;
    }

    err = system_set_read_only(staged_path, 1);
    if (err != CUP_OK) {
        system_remove_file(staged_path);
        return err;
    }

    err = commit_asset(staged_path, manifest_path, "manifest");
    if (err != CUP_OK) {
        if (err != CUP_ERR_COMMIT) {
            system_remove_file(staged_path);
        }
        return err;
    }

    printf("Restored official package manifest.\n");
    return CUP_OK;
}

static const char *find_binary_asset(const char *host) {
    if (strcmp(host, "linux-x64") == 0) {
        return "cup-linux-x64";
    }

    if (strcmp(host, "linux-arm64") == 0) {
        return "cup-linux-arm64";
    }

    if (strcmp(host, "macos-x64") == 0) {
        return "cup-macos-x64";
    }

    if (strcmp(host, "macos-arm64") == 0) {
        return "cup-macos-arm64";
    }

    if (strcmp(host, "windows-x64") == 0) {
        return "cup-windows-x64.exe";
    }

    return NULL;
}

static CupError repair_binary(void) {
    CupError err;
    char binary_path[MAX_PATH_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    const char *asset_name;
    int is_regular_file;
    int is_executable;

    err = layout_get_binary_path(binary_path, sizeof(binary_path));
    if (err != CUP_OK) {
        return err;
    }

    err = platform_get_host(host_platform, sizeof(host_platform));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_regular_file(binary_path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }

    if (!is_regular_file) {
        asset_name = find_binary_asset(host_platform);
        if (asset_name == NULL) {
            return CUP_ERR_NOT_AVAILABLE;
        }

        printf("Restoring canonical cup executable.\n");
        return restore_asset(binary_path, NULL, asset_name,
            REPAIR_ASSET_EXECUTABLE);
    }

    err = system_is_executable(binary_path, &is_executable);
    if (err != CUP_OK || !is_executable) {
        if (system_set_executable(binary_path, 1) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }

        printf("Restored executable permissions on canonical cup executable.\n");
    }

    return CUP_OK;
}

static CupError repair_uninstall_script(void) {
    CupError err;
    char script_path[MAX_PATH_LEN];
    int is_regular_file;
    int is_read_only;

    err = layout_get_uninstall_path(script_path, sizeof(script_path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_regular_file(script_path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }

    if (!is_regular_file) {
        printf("Restoring uninstall script.\n");
#if defined(_WIN32)
        return restore_asset(script_path, CUP_DEVELOPMENT_UNINSTALL_PATH,
            CUP_UNINSTALL_FILENAME, REPAIR_ASSET_READ_ONLY);
#else
        return restore_asset(script_path, CUP_DEVELOPMENT_UNINSTALL_PATH,
            CUP_UNINSTALL_FILENAME,
            REPAIR_ASSET_EXECUTABLE | REPAIR_ASSET_READ_ONLY);
#endif
    }

#if !defined(_WIN32)
    {
        int is_executable;

        err = system_is_executable(script_path, &is_executable);
        if (err != CUP_OK || !is_executable) {
            if (system_set_executable(script_path, 1) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored executable permissions on uninstall script.\n");
        }
    }
#endif

    err = system_is_read_only(script_path, &is_read_only);
    if (err != CUP_OK || !is_read_only) {
        if (system_set_read_only(script_path, 1) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }

        printf("Restored read-only protection on uninstall script.\n");
    }

    return CUP_OK;
}

static CupError repair_bootstrap_assets(void) {
    CupError err;

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

// TRANSACTION RECOVERY
static CupError move_staged_package(const PackageIdentity *package,
    const char *staged_path, const char *install_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    err = layout_ensure_package_parent(package);
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    err = system_move_path(staged_path, install_path, &commit_state);
    return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError remove_transaction_path(const char *path, int exists) {
    if (!exists) {
        return CUP_OK;
    }

    return filesystem_remove_tree(path) == CUP_OK
        ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError recover_installed_package(const Transaction *transaction,
    const char *install_path, int install_exists,
    const char *staged_path, int staged_exists) {
    CupError err;

    if (!install_exists) {
        if (!staged_exists) {
            return CUP_ERR_TRANSACTION;
        }

        err = move_staged_package(&transaction->package,
            staged_path, install_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    return remove_transaction_path(staged_path, staged_exists);
}

static CupError recover_absent_package(TransactionOperation operation,
    const char *install_path, int install_exists,
    const char *staged_path, int staged_exists) {
    CupError err;

    if (operation == TRANSACTION_INSTALL) {
        err = remove_transaction_path(install_path, install_exists);
        if (err != CUP_OK) {
            return err;
        }
        return remove_transaction_path(staged_path, staged_exists);
    }

    err = remove_transaction_path(staged_path, staged_exists);
    if (err != CUP_OK) {
        return err;
    }
    return remove_transaction_path(install_path, install_exists);
}

static CupError recover_transaction(const Transaction *transaction,
    CupState *state) {
    CupError err;
    char entry[MAX_ENTRY_LEN];
    char install_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_installed;
    int install_exists;
    int staged_exists;

    if (transaction == NULL || state == NULL ||
        (transaction->operation != TRANSACTION_INSTALL &&
            transaction->operation != TRANSACTION_REMOVE)) {
        return CUP_ERR_TRANSACTION;
    }

    if (entry_build(entry, sizeof(entry), transaction->package.tool,
        transaction->package.version) != CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path),
            &transaction->package) != CUP_OK ||
        transaction_get_tmp_path(transaction, staged_path,
            sizeof(staged_path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    is_installed = state_find_installed(state,
        transaction->package.component,
        transaction->package.host_platform,
        transaction->package.target_platform, entry) != -1;

    if (system_path_exists(install_path, &install_exists) != CUP_OK ||
        system_path_exists(staged_path, &staged_exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (is_installed) {
        err = recover_installed_package(transaction,
            install_path, install_exists, staged_path, staged_exists);
    } else {
        err = recover_absent_package(transaction->operation,
            install_path, install_exists, staged_path, staged_exists);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = transaction_clear();
    if (err == CUP_OK) {
        printf("Recovered interrupted %s transaction for %s@%s.\n",
            transaction_operation_name(transaction->operation),
            transaction->package.tool, transaction->package.version);
    }

    return err;
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
    if (err == CUP_OK) {
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

    err = recover_transaction(&context->transaction, &context->state);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: interrupted transaction cannot be repaired safely.\n");
    }

    return err;
}

static CupError repair_bootstrap(void) {
    CupError err;
    int installed_exists;
    int development_available;

    err = installed_bootstrap_exists(&installed_exists);
    if (err != CUP_OK) {
        return err;
    }

    err = development_bootstrap_available(&development_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!installed_exists && development_available) {
        printf("Using development bootstrap files from the repository.\n");
        return CUP_OK;
    }

    err = layout_ensure_bootstrap();
    if (err != CUP_OK) {
        return err;
    }

    return repair_bootstrap_assets();
}

static CupError repair_packages(RepairContext *context) {
    CupError err;

    err = package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    if (context->packages.invalid_count > 0) {
        printf("Warning: %zu invalid or ambiguous package path(s) "
            "were left unchanged.\n", context->packages.invalid_count);
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

    err = layout_ensure_runtime();
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
