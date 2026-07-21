/*
 * Runs the ordered deterministic reconciliation pipeline: transaction recovery, CUP asset
 * restoration, package scanning, state rebuilding, quarantine, cleanup and selector-point
 * repair.
 */

#include "commands.h"
#include "download.h"

#include "cup_assets.h"
#include "checksum.h"

#include "package_selector.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "cup_update_journal.h"
#include "cup_update_helper.h"
#include "runtime_journal.h"
#include "text.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

typedef unsigned int RepairAssetFlags;

enum {
    REPAIR_ASSET_EXECUTABLE = 1u << 0,
    REPAIR_ASSET_READ_ONLY = 1u << 1
};

/* Shared repair helpers. */
#if CUP_VERSION_OFFICIAL
/* Official-asset restoration. Each file is downloaded, verified and committed independently so the
 * phase is restartable. */
static CupError create_repair_temp(char *path, size_t path_size) {
    char staging_dir[MAX_PATH_LEN];
    FILE *file = NULL;

    if (layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK ||
        system_create_temp_file(staging_dir, "repair", path, path_size, &file) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    if (fclose(file) != 0) {
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}
#endif

static CupError download_asset(const char *asset_name, char *path, size_t path_size) {
#if !CUP_VERSION_OFFICIAL
    (void)asset_name;
    (void)path;
    (void)path_size;
    fprintf(stderr,
            "Error: a development build cannot restore official CUP assets "
            "assets. Run repair with the installed official cup binary or "
            "run the official installer.\n");
    return CUP_ERR_NOT_AVAILABLE;
#else
    char release_url[MAX_CATALOG_URL_LEN];
    char url[MAX_CATALOG_URL_LEN];

    if (text_format(release_url,
                    sizeof(release_url),
                    CUP_RELEASE_VERSIONED_URL_TEMPLATE,
                    CUP_VERSION_BASE) != CUP_OK ||
        text_format(url, sizeof(url), "%s/%s", release_url, asset_name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (create_repair_temp(path, path_size) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return download_file(url, path, DOWNLOAD_VALIDATE_NONEMPTY);
#endif
}

static CupError apply_asset_permissions(const char *path, RepairAssetFlags flags) {
    if ((flags & REPAIR_ASSET_EXECUTABLE) != 0 && system_set_executable(path, 1) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if ((flags & REPAIR_ASSET_READ_ONLY) != 0 && system_set_read_only(path, 1) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError restore_asset_backup(const char *backup_path, const char *destination) {
    SystemCommitState restore_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    err = system_move_path(backup_path, destination, &restore_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    return CUP_ERR_ROLLBACK;
}

static CupError commit_asset(const char *staged_path,
                             const char *destination,
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
        err = filesystem_backup_invalid(destination, backup_path, sizeof(backup_path));
        if (err != CUP_OK) {
            return err;
        }
        has_backup = 1;
    }

    err = system_replace_file(staged_path, destination, &commit_state);
    if (err == CUP_OK) {
        if (has_backup) {
            printf("Preserved invalid %s as '%s'.\n", backup_description, backup_path);
        }
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_APPLIED) {
        if (has_backup) {
            printf("Preserved invalid %s as '%s'.\n", backup_description, backup_path);
        }
        return CUP_ERR_COMMIT;
    }

    if (has_backup && restore_asset_backup(backup_path, destination) != CUP_OK) {
        fprintf(stderr,
                "Error: replacement failed and the previous %s could not be restored.\n",
                backup_description);
        return CUP_ERR_ROLLBACK;
    }

    return err;
}

static CupError restore_asset(const char *destination,
                              const char *asset_name,
                              const char *checksum_path,
                              RepairAssetFlags flags) {
    CupError err;
    char staged_path[MAX_PATH_LEN];
    int matches;

    err = download_asset(asset_name, staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }

    err = cup_assets_verify_asset(checksum_path, asset_name, staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        fprintf(stderr, "Error: checksum verification failed for '%s'.\n", asset_name);
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
                                     const char *asset_name,
                                     const char *const *required_assets,
                                     size_t required_count,
                                     int force_refresh) {
    CupError err;
    char staged_path[MAX_PATH_LEN];
    int is_regular;
    int is_read_only;

    err = system_is_regular_file(destination, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (!force_refresh && is_regular &&
        checksum_validate_assets(destination, required_assets, required_count) == CUP_OK) {
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
    if (checksum_validate_assets(staged_path, required_assets, required_count) != CUP_OK ||
        system_set_read_only(staged_path, 1) != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    if (is_regular) {
        char current_hash[SHA256_HEX_LENGTH + 1];
        char staged_hash[SHA256_HEX_LENGTH + 1];

        if (checksum_sha256_file(staged_path, staged_hash, sizeof(staged_hash)) != CUP_OK) {
            system_remove_file(staged_path);
            return CUP_ERR_FILESYSTEM;
        }
        if (checksum_sha256_file(destination, current_hash, sizeof(current_hash)) == CUP_OK &&
            strcmp(current_hash, staged_hash) == 0) {
            int read_only;

            if (system_remove_file(staged_path) != CUP_OK ||
                system_is_read_only(destination, &read_only) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }
            return read_only ? CUP_OK : system_set_read_only(destination, 1);
        }
    }

    err = commit_asset(staged_path, destination, "checksum file");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }
    return err;
}

static CupError repair_cup_assets_checksums(void) {
    CupError err;
    char common_path[MAX_PATH_LEN];
    char platform_path[MAX_PATH_LEN];
    char platform_name[MAX_IDENTIFIER_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
    const char *common_assets[] = {CUP_PACKAGES_FILENAME, CUP_INSTALL_POLICY_FILENAME};
    const char *platform_assets[3];

    if (layout_get_common_checksums_path(common_path, sizeof(common_path)) != CUP_OK ||
        layout_get_platform_checksums_path(platform_path, sizeof(platform_path)) != CUP_OK ||
        cup_assets_platform_checksums_name(platform_name, sizeof(platform_name)) != CUP_OK ||
        cup_assets_binary_asset_name(binary_asset, sizeof(binary_asset)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    platform_assets[2] = CUP_RELEASE_METADATA_FILENAME;

    err = repair_checksum_file(common_path,
                               CUP_COMMON_CHECKSUMS_FILENAME,
                               common_assets,
                               sizeof(common_assets) / sizeof(common_assets[0]),
                               0);
    if (err != CUP_OK) {
        return err;
    }
    return repair_checksum_file(platform_path,
                                platform_name,
                                platform_assets,
                                sizeof(platform_assets) / sizeof(platform_assets[0]),
                                0);
}

static CupError refresh_common_checksums(void) {
    char path[MAX_PATH_LEN];
    const char *assets[] = {CUP_PACKAGES_FILENAME, CUP_INSTALL_POLICY_FILENAME};

    if (layout_get_common_checksums_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    return repair_checksum_file(
        path, CUP_COMMON_CHECKSUMS_FILENAME, assets, sizeof(assets) / sizeof(assets[0]), 1);
}

static CupError refresh_platform_checksums(void) {
    char path[MAX_PATH_LEN];
    char name[MAX_IDENTIFIER_LEN];
    char binary[MAX_IDENTIFIER_LEN];
    const char *assets[3];

    if (layout_get_platform_checksums_path(path, sizeof(path)) != CUP_OK ||
        cup_assets_platform_checksums_name(name, sizeof(name)) != CUP_OK ||
        cup_assets_binary_asset_name(binary, sizeof(binary)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    assets[0] = binary;
    assets[1] = CUP_UNINSTALL_FILENAME;
    assets[2] = CUP_RELEASE_METADATA_FILENAME;
    return repair_checksum_file(path, name, assets, sizeof(assets) / sizeof(assets[0]), 1);
}

/* Verified CUP asset restoration. */
static CupError repair_package_catalog(void) {
    PackageCatalog catalog;
    CupError err;
    char package_catalog_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_regular;
    int matches;
    int is_read_only;

    if (layout_get_package_catalog_path(package_catalog_path, sizeof(package_catalog_path)) !=
            CUP_OK ||
        layout_get_common_checksums_path(checksums_path, sizeof(checksums_path)) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    err = system_is_regular_file(package_catalog_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular &&
        cup_assets_verify_asset(
            checksums_path, CUP_PACKAGES_FILENAME, package_catalog_path, &matches) == CUP_OK &&
        matches) {
        package_catalog_init(&catalog);
        err = package_catalog_load_installed(&catalog);
        package_catalog_free(&catalog);
        if (err == CUP_OK) {
            err = system_is_read_only(package_catalog_path, &is_read_only);
            if (err != CUP_OK) {
                return err;
            }
            if (!is_read_only) {
                printf("Restoring read-only protection on packages.cfg.\n");
                return system_set_read_only(package_catalog_path, 1);
            }
            return CUP_OK;
        }
    }

    err = refresh_common_checksums();
    if (err != CUP_OK) {
        return err;
    }
    err = download_asset(CUP_PACKAGES_FILENAME, staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }
    err = cup_assets_verify_asset(checksums_path, CUP_PACKAGES_FILENAME, staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    package_catalog_init(&catalog);
    err = package_catalog_load_path(&catalog, staged_path, PACKAGE_CATALOG_SOURCE_INSTALLED);
    package_catalog_free(&catalog);
    if (err != CUP_OK || system_set_read_only(staged_path, 1) != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_CATALOG;
    }

    err = commit_asset(staged_path, package_catalog_path, "catalog");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }
    if (err == CUP_OK) {
        printf("Restored official package catalog.\n");
    }
    return err;
}

static CupError repair_install_policy(void) {
    InstallPolicy config;
    CupError err;
    char config_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_regular;
    int matches;
    int is_read_only;

    if (layout_get_install_policy_path(config_path, sizeof(config_path)) != CUP_OK ||
        layout_get_common_checksums_path(checksums_path, sizeof(checksums_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(config_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular &&
        cup_assets_verify_asset(
            checksums_path, CUP_INSTALL_POLICY_FILENAME, config_path, &matches) == CUP_OK &&
        matches) {
        install_policy_init(&config);
        err = install_policy_load_path(&config, config_path, INSTALL_POLICY_SOURCE_INSTALLED);
        if (err == CUP_OK) {
            err = system_is_read_only(config_path, &is_read_only);
            if (err != CUP_OK) {
                return err;
            }
            if (!is_read_only) {
                printf("Restoring read-only protection on install.cfg.\n");
                return system_set_read_only(config_path, 1);
            }
            return CUP_OK;
        }
    }

    err = refresh_common_checksums();
    if (err != CUP_OK) {
        return err;
    }
    err = download_asset(CUP_INSTALL_POLICY_FILENAME, staged_path, sizeof(staged_path));
    if (err != CUP_OK) {
        return err;
    }
    err =
        cup_assets_verify_asset(checksums_path, CUP_INSTALL_POLICY_FILENAME, staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    install_policy_init(&config);
    err = install_policy_load_path(&config, staged_path, INSTALL_POLICY_SOURCE_INSTALLED);
    if (err != CUP_OK || system_set_read_only(staged_path, 1) != CUP_OK) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    err = commit_asset(staged_path, config_path, "install configuration");
    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        system_remove_file(staged_path);
    }
    if (err == CUP_OK) {
        printf("Restored official installation configuration.\n");
    }
    return err;
}

static CupError repair_binary(void) {
    CupError err;
    char binary_path[MAX_PATH_LEN];
    char checksums_path[MAX_PATH_LEN];
    char asset_name[MAX_IDENTIFIER_LEN];
    int is_regular;
    int is_executable;
    int matches;

    if (layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        layout_get_platform_checksums_path(checksums_path, sizeof(checksums_path)) != CUP_OK ||
        cup_assets_binary_asset_name(asset_name, sizeof(asset_name)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(binary_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular &&
        cup_assets_verify_asset(checksums_path, asset_name, binary_path, &matches) == CUP_OK &&
        matches) {
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

    err = refresh_platform_checksums();
    if (err != CUP_OK) {
        return err;
    }

#if defined(_WIN32)
    fprintf(stderr,
            "Error: the running cup executable is missing or altered. "
            "Run the official installer to replace it safely on Windows.\n");
    return CUP_ERR_VALIDATION;
#else
    printf("Restoring canonical cup executable.\n");
    return restore_asset(binary_path, asset_name, checksums_path, REPAIR_ASSET_EXECUTABLE);
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
        layout_get_platform_checksums_path(checksums_path, sizeof(checksums_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(script_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular ||
        cup_assets_verify_asset(checksums_path, CUP_UNINSTALL_FILENAME, script_path, &matches) !=
            CUP_OK ||
        !matches) {
        err = refresh_platform_checksums();
        if (err != CUP_OK) {
            return err;
        }
        printf("Restoring uninstall script.\n");
#if defined(_WIN32)
        return restore_asset(
            script_path, CUP_UNINSTALL_FILENAME, checksums_path, REPAIR_ASSET_READ_ONLY);
#else
        return restore_asset(script_path,
                             CUP_UNINSTALL_FILENAME,
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

static CupError repair_cup_assets_generation(void) {
    CupError err;

    err = repair_cup_assets_checksums();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_binary();
    if (err != CUP_OK) {
        return err;
    }

    err = cup_update_helper_prepare();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_uninstall_script();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_package_catalog();
    if (err != CUP_OK) {
        return err;
    }

    return repair_install_policy();
}

/* Package and state reconciliation. */
/* State reconciliation. Valid current-host packages may be adopted; foreign-host and ambiguous
 * evidence is preserved. */
static CupError remove_stale_installed_entries(CupState *state,
                                               const PackageList *packages,
                                               const char *current_host,
                                               int *state_changed) {
    size_t index = 0;

    while (index < state->installed_count) {
        PackageIdentity identity = state->installed[index];
        char selector[MAX_SELECTOR_LEN];
        CupError err;

        err = package_identity_validate(&identity);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }
        if (strcmp(identity.host_platform, current_host) != 0) {
            index++;
            continue;
        }
        if (package_list_contains(packages, &identity)) {
            index++;
            continue;
        }

        err = package_identity_format_selector(&identity, selector, sizeof(selector));
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }
        err = state_clear_matching_active(state, &identity);
        if (err != CUP_OK) {
            return err;
        }
        err = state_remove_installed(state, &identity);
        if (err != CUP_OK) {
            return err;
        }

        printf("Removed stale state record '%s:%s'.\n", identity.component, selector);
        *state_changed = 1;
    }

    return CUP_OK;
}

static CupError adopt_scanned_packages(CupState *state,
                                       const PackageList *packages,
                                       int *state_changed) {
    size_t i;

    for (i = 0; i < packages->count; ++i) {
        const PackageIdentity *package = &packages->items[i];
        char selector[MAX_SELECTOR_LEN];
        char install_path[MAX_PATH_LEN];
        int is_read_only;

        if (package_identity_format_selector(package, selector, sizeof(selector)) != CUP_OK) {
            continue;
        }

        if (state_find_installed(state, package) == -1) {
            CupError err = state_add_installed(state, package);
            if (err != CUP_OK) {
                return err;
            }

            printf("Adopted valid package '%s:%s' into state.txt.\n", package->component, selector);
            *state_changed = 1;
        }

        if (layout_build_install_path(install_path, sizeof(install_path), package) == CUP_OK &&
            (package_metadata_is_read_only(install_path, &is_read_only) != CUP_OK ||
             !is_read_only)) {
            if (package_set_metadata_read_only(install_path) != CUP_OK) {
                return CUP_ERR_FILESYSTEM;
            }

            printf("Restored read-only protection for %s@%s metadata.\n",
                   package->tool,
                   package->version);
        }
    }

    return CUP_OK;
}

static CupError remove_stale_active(CupState *state, const char *current_host, int *state_changed) {
    size_t index = 0;

    while (index < state->active_count) {
        PackageIdentity identity = state->active[index];
        PackageScope scope;

        if (strcmp(identity.host_platform, current_host) != 0 ||
            state_find_installed(state, &identity) != -1) {
            index++;
            continue;
        }

        if (package_identity_get_scope(&identity, &scope) != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }
        state_clear_active(state, &scope);
        printf("Removed stale default for component '%s'.\n", identity.component);
        *state_changed = 1;
    }

    return CUP_OK;
}

static CupError reconcile_state(CupState *state,
                                const PackageList *packages,
                                const char *current_host,
                                int *state_changed) {
    CupError err;

    err = remove_stale_installed_entries(state, packages, current_host, state_changed);
    if (err != CUP_OK) {
        return err;
    }

    err = adopt_scanned_packages(state, packages, state_changed);
    if (err != CUP_OK) {
        return err;
    }

    return remove_stale_active(state, current_host, state_changed);
}

/* Ordered repair command. */
typedef struct {
    SystemLock lock;
    CupState state;
    StateFileStatus state_status;
    PackageList packages;
    PackageTransaction package_transaction;
    CupUpdateJournal cup_update_journal;
    char current_host[MAX_PLATFORM_LEN];
    int state_changed;
    int preserve_staging;
} RepairContext;

/* Recovery context. State and journal status are collected before repair decides which later phases
 * are safe. */
static void repair_context_init(RepairContext *context) {
    memset(context, 0, sizeof(*context));
    package_transaction_init(&context->package_transaction);
    cup_update_journal_init(&context->cup_update_journal);
}

static CupError repair_load_state(RepairContext *context) {
    RuntimeJournalKind journal_kind;
    CupError load_error;
    CupError err;
    char state_path[MAX_PATH_LEN];
    char backup_path[MAX_PATH_LEN];

    err = runtime_journal_detect(&journal_kind);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: transaction.txt is invalid; repair preserved it in place "
                "and did not modify state or packages.\n");
        context->preserve_staging = 1;
        return CUP_ERR_TRANSACTION;
    }

    load_error = state_load(&context->state, &context->state_status);
    if (load_error == CUP_OK && context->state_status == STATE_FILE_LOADED &&
        state_validate(&context->state) == CUP_OK) {
        return CUP_OK;
    }

    if (journal_kind == RUNTIME_JOURNAL_PACKAGE) {
        fprintf(stderr,
                "Error: state.txt is missing or invalid while a package "
                "transaction is pending; the commit point is ambiguous and all "
                "evidence was preserved.\n");
        context->preserve_staging = 1;
        return CUP_ERR_TRANSACTION;
    }

    if (load_error == CUP_OK && context->state_status == STATE_FILE_MISSING) {
        memset(&context->state, 0, sizeof(context->state));
        context->state_changed = 1;
        return CUP_OK;
    }

    err = layout_get_state_path(state_path, sizeof(state_path));
    if (err != CUP_OK)
        return CUP_ERR_STATE_LOAD;
    err = filesystem_backup_invalid(state_path, backup_path, sizeof(backup_path));
    if (err != CUP_OK)
        return CUP_ERR_STATE_LOAD;

    printf("Preserved invalid state as '%s'.\n", backup_path);
    memset(&context->state, 0, sizeof(context->state));
    context->state_status = STATE_FILE_MISSING;
    context->state_changed = 1;
    return CUP_OK;
}

static CupError repair_pending_transaction(RepairContext *context) {
    RuntimeJournalKind journal_kind;
    PackageTransactionStatus package_status;
    CupUpdateJournalStatus update_status;
    CupError err;
    err = runtime_journal_detect(&journal_kind);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: transaction.txt is invalid and remains the canonical "
                "blocker; repair made no destructive changes.\n");
        context->preserve_staging = 1;
        return CUP_ERR_TRANSACTION;
    }
    if (journal_kind == RUNTIME_JOURNAL_MISSING) {
        return CUP_OK;
    }

    if (journal_kind == RUNTIME_JOURNAL_PACKAGE) {
        err = package_transaction_load(&context->package_transaction, &package_status);
        if (err == CUP_OK && package_status == PACKAGE_TRANSACTION_LOADED &&
            context->state_status == STATE_FILE_LOADED &&
            state_validate(&context->state) == CUP_OK) {
            err = package_transaction_recover(&context->package_transaction, &context->state);
        } else {
            err = CUP_ERR_TRANSACTION;
        }
    } else {
        err = cup_update_journal_load(&context->cup_update_journal, &update_status);
        if (err == CUP_OK && update_status == CUP_UPDATE_JOURNAL_LOADED) {
            err = cup_update_journal_recover(&context->cup_update_journal);
        } else {
            err = CUP_ERR_TRANSACTION;
        }
    }

    if (err != CUP_OK) {
        fprintf(stderr, "Error: interrupted operation cannot be repaired safely.\n");
    }
    return err;
}

static CupError repair_cup_assets(void) {
    CupAssetsInspection inspection;
    CupError err;

    err = cup_assets_inspect(&inspection);
    if (err != CUP_OK) {
        return err;
    }
    if (!cup_assets_has_installed_assets(&inspection) &&
        cup_assets_development_is_valid(&inspection)) {
        printf("Using development CUP assets from the repository.\n");
        return CUP_OK;
    }

    err = layout_ensure_cup_assets();
    if (err != CUP_OK) {
        return err;
    }
    return repair_cup_assets_generation();
}

/* Package-tree repair. A complete representable scan is required before quarantine or state changes
 * begin. */
static CupError quarantine_invalid_packages(PackageList *packages, int *quarantined_any) {
    size_t i;

    *quarantined_any = 0;
    for (i = 0; i < packages->issue_count; ++i) {
        const PackageIssue *issue = &packages->issues[i];

        if (issue->can_quarantine) {
            CupError err;
            char recovery_path[MAX_PATH_LEN];

            err = package_quarantine(issue, recovery_path, sizeof(recovery_path));
            if (err != CUP_OK) {
                return err;
            }

            printf("Quarantined invalid package '%s' as '%s'.\n", issue->path, recovery_path);
            *quarantined_any = 1;
            continue;
        }

        printf("Warning: package path '%s' was left unchanged: %s.\n",
               issue->path,
               package_issue_reason_name(issue->reason));
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

static CupError require_representable_package_scan(const PackageList *packages) {
    if (packages->total_count <= MAX_INSTALLED) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: %zu valid packages were found, but state.txt supports at most "
            "%d installed entries; repair did not modify packages or state.\n",
            packages->total_count,
            MAX_INSTALLED);
    return CUP_ERR_INCONSISTENT_STATE;
}

static CupError repair_packages(RepairContext *context) {
    CupError err;
    int quarantined_any;

    err = package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    if (context->packages.foreign_host_count > 0) {
        printf(
            "Preserved %zu foreign-host package tree(s) without adopting or quarantining them.\n",
            context->packages.foreign_host_count);
    }

    err = require_complete_package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    err = require_representable_package_scan(&context->packages);
    if (err != CUP_OK) {
        return err;
    }

    err = quarantine_invalid_packages(&context->packages, &quarantined_any);
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

    return reconcile_state(
        &context->state, &context->packages, context->current_host, &context->state_changed);
}

static CupError repair_save_state(const RepairContext *context) {
    CupError err;

    if (context->state_status == STATE_FILE_LOADED && !context->state_changed) {
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

    err = cup_assets_uninstall_is_pending(&pending);
    if (err != CUP_OK) {
        return err;
    }
    if (!pending) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: cup uninstall is in progress or did not finish. "
            "Run the installer again if the marker is stale.\n");
    return CUP_ERR_LOCK;
}

static CupError repair_cleanup_staging(const RepairContext *context) {
    char staging_dir[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];

    if (context->preserve_staging) {
        return CUP_OK;
    }

    if (layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK ||
        layout_get_transaction_path(transaction_path, sizeof(transaction_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return filesystem_clear_directory(staging_dir, transaction_path);
}

/* Ordered repair pipeline. Every phase is idempotent, and an ambiguous phase prevents all later
 * mutations. */
CupError command_repair(void) {
    RepairContext context;
    CupError err;
    char lock_path[MAX_PATH_LEN];

    repair_context_init(&context);
    printf("==> Repairing cup...\n");

    err = platform_get_host(context.current_host, sizeof(context.current_host));
    if (err != CUP_OK) {
        return err;
    }

    err = layout_ensure_root();
    if (err != CUP_OK) {
        return err;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_lock_acquire(&context.lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
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
        err = repair_cup_assets();
    }
    if (err == CUP_OK) {
        err = repair_packages(&context);
    }
    if (err == CUP_OK) {
        err = repair_save_state(&context);
    }
    if (err == CUP_OK) {
        WrapperPlan wrappers;

        wrapper_plan_init(&wrappers);
        err = wrapper_plan_build(&wrappers, &context.state);
        if (err == CUP_OK) {
            err = wrapper_plan_apply(&wrappers);
        }
        wrapper_plan_free(&wrappers);
        if (err == CUP_OK) {
            printf("Rebuilt managed wrappers.\n");
        }
    }
    if (err == CUP_OK) {
        err = repair_cleanup_staging(&context);
    }

    if (err == CUP_OK) {
        printf("Repair completed.\n");
    }

    system_lock_release(&context.lock);
    return err;
}
