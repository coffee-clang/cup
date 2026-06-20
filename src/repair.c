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
#include "util.h"

#include <stdio.h>
#include <string.h>

#define BOOTSTRAP_URL "https://github.com/coffee-clang/cup/releases/download/cup-bootstrap"

#if defined(_WIN32)
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup-windows.ps1"
#else
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup.sh"
#endif

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

    err = system_is_regular_file(DEVELOPMENT_UNINSTALL_PATH, &uninstall_exists);
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

static CupError download_asset(const char *asset, char *temporary, size_t temporary_size) {
    CupError err;
    char tmp_dir[MAX_PATH_LEN];
    char pid[MAX_NAME_LEN];
    char url[MAX_MANIFEST_URL_LEN];

    if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        system_get_process_id(pid, sizeof(pid)) != CUP_OK ||
        checked_snprintf(temporary, temporary_size,
            "%s/repair-%s-%s", tmp_dir, asset, pid) != CUP_OK ||
        checked_snprintf(url, sizeof(url), "%s/%s", BOOTSTRAP_URL, asset) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = filesystem_remove_tree(temporary);
    if (err != CUP_OK) {
        return err;
    }

    return fetch_resource(url, temporary);
}

static CupError restore_asset(const char *destination, const char *local_source,
    const char *asset, int executable, int read_only) {
    CupError err;
    char temporary[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    int exists;

    if (!is_empty_string(local_source) &&
        system_is_regular_file(local_source, &exists) == CUP_OK && exists) {
        char tmp_dir[MAX_PATH_LEN];
        char pid[MAX_NAME_LEN];

        if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
            system_get_process_id(pid, sizeof(pid)) != CUP_OK ||
            checked_snprintf(temporary, sizeof(temporary),
                "%s/repair-%s-%s", tmp_dir, asset, pid) != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }

        err = filesystem_remove_tree(temporary);
        if (err == CUP_OK) {
            err = system_copy_file(local_source, temporary);
        }
    } else {
        err = download_asset(asset, temporary, sizeof(temporary));
    }

    if (err != CUP_OK) {
        return err;
    }

    if (executable && system_set_executable(temporary, 1) != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    if (read_only && system_set_read_only(temporary, 1) != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    err = system_path_exists(destination, &exists);
    if (err != CUP_OK) {
        system_remove_file(temporary);
        return err;
    }

    if (exists) {
        system_set_read_only(destination, 0);

        err = filesystem_backup_invalid(destination, backup, sizeof(backup));
        if (err != CUP_OK) {
            system_remove_file(temporary);
            return err;
        }

        printf("Preserved invalid file as '%s'.\n", backup);
    }

    err = system_replace_file(temporary, destination);
    if (err != CUP_OK) {
        system_remove_file(temporary);
    }

    return err;
}

// OFFICIAL FILES
static CupError repair_manifest(void) {
    Manifest manifest;
    CupError err;
    char path[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    int exists;
    int read_only;

    manifest_init(&manifest);

    if (layout_get_manifest_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists && manifest_load_installed(&manifest) == CUP_OK) {
        manifest_free(&manifest);

        if (system_is_read_only(path, &read_only) != CUP_OK || !read_only) {
            printf("Restoring read-only protection on packages.cfg.\n");
            return system_set_read_only(path, 1);
        }

        return CUP_OK;
    }

    manifest_free(&manifest);

    manifest_init(&manifest);
    err = manifest_load_development(&manifest);

    if (err == CUP_OK) {
        char tmp_dir[MAX_PATH_LEN];
        char pid[MAX_NAME_LEN];

        if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
            system_get_process_id(pid, sizeof(pid)) != CUP_OK ||
            checked_snprintf(temporary, sizeof(temporary),
                "%s/repair-packages-%s", tmp_dir, pid) != CUP_OK ||
            system_copy_file(manifest.path, temporary) != CUP_OK) {
            manifest_free(&manifest);
            return CUP_ERR_MANIFEST;
        }

        manifest_free(&manifest);
    } else {
        manifest_free(&manifest);
        err = download_asset("packages.cfg", temporary, sizeof(temporary));
        if (err != CUP_OK) {
            return err;
        }
    }

    manifest_init(&manifest);
    err = manifest_load_path(&manifest, temporary, MANIFEST_SOURCE_INSTALLED);
    manifest_free(&manifest);

    if (err != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_MANIFEST;
    }

    if (system_set_read_only(temporary, 1) != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    if (system_path_exists(path, &exists) != CUP_OK) {
        system_remove_file(temporary);
        return CUP_ERR_FILESYSTEM;
    }

    if (exists) {
        system_set_read_only(path, 0);

        err = filesystem_backup_invalid(path, backup, sizeof(backup));
        if (err != CUP_OK) {
            system_remove_file(temporary);
            return err;
        }

        printf("Preserved invalid manifest as '%s'.\n", backup);
    }

    err = system_replace_file(temporary, path);
    if (err == CUP_OK) {
        printf("Restored official package manifest.\n");
    }

    return err;
}

static const char *get_binary_asset(const char *host) {
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

static CupError repair_assets(void) {
    CupError err;
    char path[MAX_PATH_LEN];
    char host[MAX_PLATFORM_LEN];
    const char *asset;
    int exists;

    if (layout_get_binary_path(path, sizeof(path)) != CUP_OK ||
        get_host_platform(host, sizeof(host)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (system_is_regular_file(path, &exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!exists) {
        asset = get_binary_asset(host);
        if (asset == NULL) {
            return CUP_ERR_NOT_AVAILABLE;
        }

        printf("Restoring canonical cup executable.\n");
        err = restore_asset(path, NULL, asset, 1, 0);
        if (err != CUP_OK) {
            return err;
        }
    } else {
        int executable;

        if (system_is_executable(path, &executable) != CUP_OK || !executable) {
            if (system_set_executable(path, 1) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored executable permissions on canonical cup executable.\n");
        }
    }

    if (layout_get_uninstall_path(path, sizeof(path)) != CUP_OK ||
        system_is_regular_file(path, &exists) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!exists) {
        printf("Restoring uninstall script.\n");

#if defined(_WIN32)
        err = restore_asset(path, DEVELOPMENT_UNINSTALL_PATH,
            "uninstall.ps1", 0, 1);
#else
        err = restore_asset(path, DEVELOPMENT_UNINSTALL_PATH,
            "uninstall.sh", 1, 1);
#endif

        if (err != CUP_OK) {
            return err;
        }
    } else {
        int executable;
        int read_only;

        if (system_is_executable(path, &executable) != CUP_OK || !executable) {
            if (system_set_executable(path, 1) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored executable permissions on uninstall script.\n");
        }

        if (system_is_read_only(path, &read_only) != CUP_OK || !read_only) {
            if (system_set_read_only(path, 1) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored read-only protection on uninstall script.\n");
        }
    }

    return repair_manifest();
}

// TRANSACTION RECOVERY
static CupError recover_transaction(const Transaction *transaction, CupState *state) {
    CupError err;
    char entry[MAX_ENTRY_LEN];
    char install_path[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN];
    int installed;
    int install_exists;
    int temporary_exists;

    if (entry_build(entry, sizeof(entry), transaction->package.tool,
        transaction->package.version) != CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path),
            &transaction->package) != CUP_OK ||
        transaction_get_tmp_path(transaction, temporary_path,
            sizeof(temporary_path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    installed = state_find_installed(state, transaction->package.component,
        transaction->package.host_platform, transaction->package.target_platform,
        entry) != -1;

    if (system_path_exists(install_path, &install_exists) != CUP_OK ||
        system_path_exists(temporary_path, &temporary_exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (transaction->operation == TRANSACTION_INSTALL) {
        if (installed) {
            if (!install_exists && temporary_exists) {
                err = layout_ensure_package_parent(&transaction->package);
                if (err != CUP_OK ||
                    system_move_path(temporary_path, install_path) != CUP_OK) {
                    return CUP_ERR_TRANSACTION;
                }
            } else if (!install_exists) {
                return CUP_ERR_TRANSACTION;
            }

            if (temporary_exists && filesystem_remove_tree(temporary_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }
        } else {
            if (install_exists && filesystem_remove_tree(install_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }

            if (temporary_exists && filesystem_remove_tree(temporary_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }
        }
    } else if (transaction->operation == TRANSACTION_REMOVE) {
        if (installed) {
            if (!install_exists && temporary_exists) {
                err = layout_ensure_package_parent(&transaction->package);
                if (err != CUP_OK ||
                    system_move_path(temporary_path, install_path) != CUP_OK) {
                    return CUP_ERR_TRANSACTION;
                }
            } else if (!install_exists) {
                return CUP_ERR_TRANSACTION;
            } else if (temporary_exists &&
                filesystem_remove_tree(temporary_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }
        } else {
            if (temporary_exists && filesystem_remove_tree(temporary_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }

            if (install_exists && filesystem_remove_tree(install_path) != CUP_OK) {
                return CUP_ERR_TRANSACTION;
            }
        }
    } else {
        return CUP_ERR_TRANSACTION;
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
static CupError reconcile_state(CupState *state, const PackageList *packages, int *changed) {
    size_t i = 0;

    while (i < state->installed_count) {
        PackageIdentity package;
        StateEntry entry = state->installed[i];

        if (package_identity_from_entry(&package, entry.component,
            entry.host_platform, entry.target_platform, entry.entry) != CUP_OK ||
            !package_list_contains(packages, &package)) {
            state_clear_matching_default(state, entry.component,
                entry.host_platform, entry.target_platform, entry.entry);
            state_remove_installed(state, entry.component,
                entry.host_platform, entry.target_platform, entry.entry);

            printf("Removed stale state entry '%s:%s'.\n",
                entry.component, entry.entry);
            *changed = 1;
            continue;
        }

        i++;
    }

    for (i = 0; i < packages->count; ++i) {
        const PackageIdentity *package = &packages->items[i];
        char entry[MAX_ENTRY_LEN];
        char install_path[MAX_PATH_LEN];
        int read_only;

        if (entry_build(entry, sizeof(entry), package->tool, package->version) != CUP_OK) {
            continue;
        }

        if (state_find_installed(state, package->component,
            package->host_platform, package->target_platform, entry) == -1) {
            CupError err;

            err = state_add_installed(state, package->component,
                package->host_platform, package->target_platform, entry);
            if (err != CUP_OK) {
                return err;
            }

            printf("Adopted valid package '%s:%s' into state.txt.\n",
                package->component, entry);
            *changed = 1;
        }

        if (layout_build_install_path(install_path, sizeof(install_path), package) == CUP_OK &&
            (package_info_is_read_only(install_path, &read_only) != CUP_OK || !read_only)) {
            if (package_set_info_read_only(install_path) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored read-only protection for %s@%s metadata.\n",
                package->tool, package->version);
        }
    }

    i = 0;

    while (i < state->default_count) {
        StateEntry entry = state->defaults[i];

        if (state_find_installed(state, entry.component,
            entry.host_platform, entry.target_platform, entry.entry) == -1) {
            state_clear_default(state, entry.component,
                entry.host_platform, entry.target_platform);

            printf("Removed stale default for component '%s'.\n", entry.component);
            *changed = 1;
            continue;
        }

        i++;
    }

    return CUP_OK;
}

// REPAIR COMMAND
CupError handle_repair(void) {
    SystemLock lock = {0, 0};
    CupState state;
    StateFileStatus state_status;
    PackageList packages;
    Transaction transaction;
    TransactionFileStatus transaction_status;
    CupError err;
    char lock_path[MAX_PATH_LEN];
    char state_path[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];
    char tmp_dir[MAX_PATH_LEN];
    int changed = 0;
    int skip_tmp_cleanup = 0;
    int installed_bootstrap;
    int development_bootstrap;

    transaction_init(&transaction);
    printf("==> Repairing cup...\n");

    err = layout_ensure_root();
    if (err != CUP_OK) {
        return err;
    }

    if (layout_get_lock_path(lock_path, sizeof(lock_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    err = layout_ensure_runtime();
    if (err != CUP_OK) {
        goto done;
    }

    err = state_load(&state, &state_status);
    if (err != CUP_OK) {
        err = transaction_load(&transaction, &transaction_status);
        if (err == CUP_OK && transaction_status == TRANSACTION_FILE_LOADED) {
            fprintf(stderr, "Error: state.txt is invalid while a transaction is pending; "
                "automatic recovery would be ambiguous.\n");
            err = CUP_ERR_TRANSACTION;
            goto done;
        }

        if (layout_get_state_path(state_path, sizeof(state_path)) != CUP_OK) {
            err = CUP_ERR_STATE_LOAD;
            goto done;
        }

        if (filesystem_backup_invalid(state_path, backup, sizeof(backup)) != CUP_OK) {
            err = CUP_ERR_STATE_LOAD;
            goto done;
        }

        printf("Preserved invalid state as '%s'.\n", backup);
        memset(&state, 0, sizeof(state));
        state_status = STATE_FILE_MISSING;
        changed = 1;
    }

    err = transaction_load(&transaction, &transaction_status);
    if (err != CUP_OK) {
        if (layout_get_transaction_path(transaction_path,
            sizeof(transaction_path)) != CUP_OK ||
            filesystem_backup_invalid(transaction_path,
                backup, sizeof(backup)) != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
            goto done;
        }

        printf("Preserved invalid transaction journal as '%s'; "
            "ambiguous temporary data was left untouched.\n", backup);
        skip_tmp_cleanup = 1;
    } else if (transaction_status == TRANSACTION_FILE_LOADED) {
        err = recover_transaction(&transaction, &state);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: interrupted transaction cannot be repaired safely.\n");
            goto done;
        }
    }

    err = installed_bootstrap_exists(&installed_bootstrap);
    if (err != CUP_OK) {
        goto done;
    }

    err = development_bootstrap_available(&development_bootstrap);
    if (err != CUP_OK) {
        goto done;
    }

    if (installed_bootstrap || !development_bootstrap) {
        err = layout_ensure_bootstrap();
        if (err != CUP_OK) {
            goto done;
        }

        err = repair_assets();
        if (err != CUP_OK) {
            goto done;
        }
    } else {
        printf("Using development bootstrap files from the repository.\n");
    }

    err = package_scan(&packages);
    if (err != CUP_OK) {
        goto done;
    }

    if (packages.invalid_count > 0) {
        printf("Warning: %zu invalid or ambiguous package path(s) were left unchanged.\n",
            packages.invalid_count);
    }

    err = reconcile_state(&state, &packages, &changed);
    if (err != CUP_OK) {
        goto done;
    }

    if (state_status == STATE_FILE_MISSING || changed) {
        err = state_save(&state);
        if (err != CUP_OK) {
            goto done;
        }

        printf("Saved a valid state.txt.\n");
    }

    if (!skip_tmp_cleanup) {
        if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
            layout_get_transaction_path(transaction_path,
                sizeof(transaction_path)) != CUP_OK) {
            err = CUP_ERR_FILESYSTEM;
            goto done;
        }

        err = filesystem_clear_directory(tmp_dir, transaction_path);
        if (err != CUP_OK) {
            goto done;
        }
    }

    printf("Repair completed.\n");
    err = CUP_OK;

done:
    system_lock_release(&lock);
    return err;
}
