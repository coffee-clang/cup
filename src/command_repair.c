/*
 * Reconciles interrupted operations, assets, packages, state and wrappers
 * in a deterministic order.
 */

#include "commands.h"
#include "download.h"

#include "assets.h"
#include "checksum.h"

#include "package_selector.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "interrupt.h"
#include "package_catalog.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "update_journal.h"
#include "update_helper.h"
#include "runtime_journal.h"
#include "uninstall_helper.h"
#include "uninstall_journal.h"
#include "text.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            "Error: a development build cannot restore official assets. "
            "Run repair with the installed official binary or "
            "run the official installer.\n");
    return CUP_ERR_NOT_AVAILABLE;
#else
    char release_url[MAX_CATALOG_URL_LEN];
    char url[MAX_CATALOG_URL_LEN];
    CupError override_err;

    override_err = download_copy_release_base_override(release_url, sizeof(release_url));
    if (override_err == CUP_ERR_NOT_AVAILABLE) {
        if (text_format(release_url,
                        sizeof(release_url),
                        CUP_RELEASE_VERSIONED_URL_TEMPLATE,
                        CUP_VERSION_BASE) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
    } else if (override_err != CUP_OK) {
        return override_err;
    }

    if (text_format(url, sizeof(url), "%s/%s", release_url, asset_name) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (create_repair_temp(path, path_size) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return download_file(url, path, DOWNLOAD_VALIDATE_NONEMPTY);
#endif
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
        char current_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
        char staged_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];

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

static CupError repair_assets_checksums(void) {
    CupError err;
    char common_path[MAX_PATH_LEN];
    char platform_path[MAX_PATH_LEN];
    char platform_name[MAX_IDENTIFIER_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
    const char *platform_assets[CUP_PLATFORM_CHECKSUM_ASSET_COUNT];

    if (layout_get_common_checksums_path(common_path, sizeof(common_path)) != CUP_OK ||
        layout_get_platform_checksums_path(platform_path, sizeof(platform_path)) != CUP_OK ||
        assets_platform_checksums_name(platform_name, sizeof(platform_name)) != CUP_OK ||
        assets_binary_asset_name(binary_asset, sizeof(binary_asset)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_RELEASE_METADATA_FILENAME;
    platform_assets[2] = CUP_COMMON_CHECKSUMS_FILENAME;

    err = repair_checksum_file(common_path,
                               CUP_COMMON_CHECKSUMS_FILENAME,
                               CUP_COMMON_CHECKSUM_ASSETS,
                               CUP_COMMON_CHECKSUM_ASSET_COUNT,
                               0);
    if (err != CUP_OK) {
        return err;
    }
    err = repair_checksum_file(platform_path,
                               platform_name,
                               platform_assets,
                               sizeof(platform_assets) / sizeof(platform_assets[0]),
                               0);
    if (err == CUP_OK) {
        int matches;

        err = checksum_verify_file(platform_path,
                                   CUP_COMMON_CHECKSUMS_FILENAME,
                                   common_path,
                                   &matches);
        if (err == CUP_OK && !matches) {
            err = repair_checksum_file(platform_path,
                                       platform_name,
                                       platform_assets,
                                       sizeof(platform_assets) / sizeof(platform_assets[0]),
                                       1);
            if (err == CUP_OK) {
                err = checksum_verify_file(platform_path,
                                           CUP_COMMON_CHECKSUMS_FILENAME,
                                           common_path,
                                           &matches);
            }
        }
        if (err == CUP_OK && !matches) {
            err = CUP_ERR_VALIDATION;
        }
    }
    return err;
}

static CupError refresh_platform_checksums(void);

static CupError refresh_common_checksums(void) {
    CupError err;
    char path[MAX_PATH_LEN];

    if (layout_get_common_checksums_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    err = repair_checksum_file(path,
                               CUP_COMMON_CHECKSUMS_FILENAME,
                               CUP_COMMON_CHECKSUM_ASSETS,
                               CUP_COMMON_CHECKSUM_ASSET_COUNT,
                               1);
    return err == CUP_OK ? refresh_platform_checksums() : err;
}

static CupError refresh_platform_checksums(void) {
    char path[MAX_PATH_LEN];
    char name[MAX_IDENTIFIER_LEN];
    char binary[MAX_IDENTIFIER_LEN];
    const char *assets[CUP_PLATFORM_CHECKSUM_ASSET_COUNT];

    if (layout_get_platform_checksums_path(path, sizeof(path)) != CUP_OK ||
        assets_platform_checksums_name(name, sizeof(name)) != CUP_OK ||
        assets_binary_asset_name(binary, sizeof(binary)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    assets[0] = binary;
    assets[1] = CUP_RELEASE_METADATA_FILENAME;
    assets[2] = CUP_COMMON_CHECKSUMS_FILENAME;
    return repair_checksum_file(path, name, assets, sizeof(assets) / sizeof(assets[0]), 1);
}

/* Verified asset restoration. */
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
        checksum_verify_file(
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
    err = checksum_verify_file(checksums_path, CUP_PACKAGES_FILENAME, staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    package_catalog_init(&catalog);
    err = package_catalog_load_path(&catalog, staged_path);
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
        checksum_verify_file(
            checksums_path, CUP_INSTALL_POLICY_FILENAME, config_path, &matches) == CUP_OK &&
        matches) {
        install_policy_init(&config);
        err = install_policy_load_path(&config, config_path);
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
        checksum_verify_file(checksums_path, CUP_INSTALL_POLICY_FILENAME, staged_path, &matches);
    if (err != CUP_OK || !matches) {
        system_remove_file(staged_path);
        return CUP_ERR_VALIDATION;
    }

    install_policy_init(&config);
    err = install_policy_load_path(&config, staged_path);
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
    int matches = 0;

    if (layout_get_binary_path(binary_path, sizeof(binary_path)) != CUP_OK ||
        layout_get_platform_checksums_path(checksums_path, sizeof(checksums_path)) != CUP_OK ||
        assets_binary_asset_name(asset_name, sizeof(asset_name)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(binary_path, &is_regular);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular) {
        err = checksum_verify_file(checksums_path, asset_name, binary_path, &matches);
        if (err != CUP_OK || !matches) {
            err = refresh_platform_checksums();
            if (err != CUP_OK) {
                return err;
            }
            err = checksum_verify_file(checksums_path, asset_name, binary_path, &matches);
        }
    }

    if (!is_regular || err != CUP_OK || !matches) {
        fprintf(stderr,
                "Error: the installed cup executable is missing or does not match the official "
                "release. Repair preserved it unchanged; run the official installer to replace "
                "it safely.\n");
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }

    err = system_is_executable(binary_path, &is_executable);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_executable) {
        err = system_set_executable(binary_path, 1);
        if (err == CUP_OK) {
            printf("Restored executable permissions on the installed cup executable.\n");
        }
    }
    return err;
}

static CupError repair_update_helper(void) {
    char helper[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    err = layout_get_update_helper_path(helper, sizeof(helper));
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(helper, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind != SYSTEM_PATH_MISSING && kind != SYSTEM_PATH_REGULAR_FILE) {
        err = filesystem_backup_invalid(helper, backup, sizeof(backup));
        if (err != CUP_OK) {
            return err;
        }
        printf("Preserved invalid native update helper as '%s'.\n", backup);
    }

    err = update_helper_prepare();
    if (err == CUP_OK) {
        printf("Regenerated native update helper from the installed executable.\n");
    }
    return err;
}

static CupError repair_assets_generation(void) {
    CupError err;

    err = repair_assets_checksums();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_binary();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_update_helper();
    if (err != CUP_OK) {
        return err;
    }

    err = repair_package_catalog();
    if (err == CUP_OK) {
        err = repair_install_policy();
    }
    if (err == CUP_OK) {
        AssetsInspection inspection;

        err = assets_inspect(&inspection);
        if (err == CUP_OK && !assets_installed_is_valid(&inspection)) {
            fprintf(stderr,
                    "Error: repaired assets do not form one complete verified generation.\n");
            err = CUP_ERR_VALIDATION;
        }
    }
    return err;
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

        err = package_identity_validate(&identity, NULL);
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
        err = state_clear_matching_default(state, &identity);
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
            CupError err = package_set_metadata_read_only(install_path);

            if (err != CUP_OK) {
                return err;
            }

            printf("Restored read-only protection for %s@%s metadata.\n",
                   package->tool,
                   package->version);
        }
    }

    return CUP_OK;
}

static CupError remove_stale_defaults(CupState *state,
                                      const char *current_host,
                                      int *state_changed) {
    size_t index = 0;
    CupError err;

    while (index < state->default_count) {
        PackageIdentity identity = state->defaults[index];
        PackageScope scope;

        if (strcmp(identity.host_platform, current_host) != 0 ||
            state_find_installed(state, &identity) != -1) {
            index++;
            continue;
        }

        if (package_identity_get_scope(&identity, &scope) != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }
        err = state_clear_default(state, &scope);
        if (err != CUP_OK) {
            return err;
        }
        printf("Removed stale default for component '%s'.\n", identity.component);
        *state_changed = 1;
    }

    return CUP_OK;
}

static CupError reconcile_state(CupState *state,
                                const PackageList *packages,
                                const char *current_host,
                                int *state_changed) {
    CupState candidate;
    CupError err;
    int candidate_changed = 0;

    if (state == NULL || packages == NULL || state_changed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    candidate = *state;

    err = remove_stale_installed_entries(
        &candidate, packages, current_host, &candidate_changed);
    if (err == CUP_OK) {
        err = adopt_scanned_packages(&candidate, packages, &candidate_changed);
    }
    if (err == CUP_OK) {
        err = remove_stale_defaults(&candidate, current_host, &candidate_changed);
    }
    if (err == CUP_OK) {
        err = state_validate(&candidate, NULL);
    }
    if (err != CUP_OK) {
        return err;
    }

    *state = candidate;
    *state_changed = *state_changed || candidate_changed;
    return CUP_OK;
}

/* Ordered repair command. */
typedef struct {
    SystemLock lock;
    CupState state;
    StateFileStatus state_status;
    SystemPathIdentity state_identity;
    PackageList packages;
    PackageTransaction package_transaction;
    UpdateJournal update_journal;
    char current_host[MAX_PLATFORM_LEN];
    int state_changed;
    int preserve_staging;
} RepairContext;

/* Recovery context. State and journal status are collected before repair decides which later phases
 * are safe. */
static void repair_context_init(RepairContext *context) {
    memset(context, 0, sizeof(*context));
    package_transaction_init(&context->package_transaction);
    update_journal_init(&context->update_journal);
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

    /* An update journal does not own state, but its complete schema must still be validated
     * before repair preserves or replaces an invalid state file. Classification by operation alone
     * is not sufficient evidence for any mutation. */
    if (journal_kind == RUNTIME_JOURNAL_UPDATE) {
        UpdateJournal journal;
        UpdateJournalStatus status;

        update_journal_init(&journal);
        err = update_journal_load(&journal, &status);
        if (err != CUP_OK || status != CUP_UPDATE_JOURNAL_LOADED) {
            fprintf(stderr,
                    "Error: the cup update journal is invalid; repair preserved it and state "
                    "without modification.\n");
            context->preserve_staging = 1;
            return CUP_ERR_TRANSACTION;
        }
    }

    load_error = state_load(
        &context->state, &context->state_status, &context->state_identity, NULL);
    if (load_error == CUP_OK && context->state_status == STATE_FILE_LOADED &&
        state_validate(&context->state, NULL) == CUP_OK) {
        return CUP_OK;
    }

    if (journal_kind == RUNTIME_JOURNAL_PACKAGE) {
        fprintf(stderr,
                "Error: state.txt is missing or invalid while a state-owning "
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
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }
    if (!context->state_identity.valid) {
        err = system_get_path_identity(state_path, &context->state_identity);
        if (err != CUP_OK) {
            return CUP_ERR_STATE_LOAD;
        }
    }
    err = filesystem_backup_invalid_if_identity(
        state_path, &context->state_identity, backup_path, sizeof(backup_path));
    if (err != CUP_OK) {
        return CUP_ERR_STATE_LOAD;
    }

    printf("Preserved invalid state as '%s'.\n", backup_path);
    memset(&context->state, 0, sizeof(context->state));
    memset(&context->state_identity, 0, sizeof(context->state_identity));
    context->state_status = STATE_FILE_MISSING;
    context->state_changed = 1;
    return CUP_OK;
}

static CupError repair_pending_transaction(RepairContext *context) {
    RuntimeJournalKind journal_kind;
    PackageTransactionStatus package_status;
    UpdateJournalStatus update_status;
    UninstallJournal uninstall_journal;
    UninstallJournalStatus uninstall_status;
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
            state_validate(&context->state, NULL) == CUP_OK) {
            err = package_transaction_recover(&context->package_transaction, &context->state);
        } else {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (journal_kind == RUNTIME_JOURNAL_UPDATE) {
        err = update_journal_load(&context->update_journal, &update_status);
        if (err == CUP_OK && update_status == CUP_UPDATE_JOURNAL_LOADED) {
            err = update_journal_recover(&context->update_journal,
                                         CUP_UPDATE_RECOVER_PRESERVE_BINARY,
                                         NULL);
        } else {
            err = CUP_ERR_TRANSACTION;
        }
    } else {
        char root[MAX_PATH_LEN];

        uninstall_journal_init(&uninstall_journal);
        err = uninstall_journal_load(&uninstall_journal, &uninstall_status);
        if (err == CUP_OK && uninstall_status == UNINSTALL_JOURNAL_LOADED) {
            err = layout_get_root(root, sizeof(root));
        } else {
            err = CUP_ERR_TRANSACTION;
        }
        if (err == CUP_OK) {
            /* A child that failed before detach can leave only its reserved token-bound native
             * copy outside the root. Remove it while this repair still owns canonical exclusivity,
             * then clear the journal. If cleanup cannot be proved, keep the journal as blocker. */
            err = uninstall_helper_remove_stale(
                root, uninstall_journal.token, &context->lock);
        }
        if (err == CUP_OK) {
            err = uninstall_journal_recover(&uninstall_journal);
        }
    }

    if (err != CUP_OK) {
        fprintf(stderr, "Error: interrupted operation cannot be repaired safely.\n");
    }
    return err;
}

static CupError repair_assets(void) {
    AssetsInspection inspection;
    CupError err;

    err = assets_inspect(&inspection);
    if (err != CUP_OK) {
        return err;
    }
    if (!assets_has_installed_assets(&inspection) &&
        assets_development_is_valid(&inspection)) {
        printf("Using development assets from the repository.\n");
        return CUP_OK;
    }

    err = layout_ensure_assets();
    if (err != CUP_OK) {
        return err;
    }
    return repair_assets_generation();
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

static CupError require_projected_state_capacity(const CupState *state,
                                                 const PackageList *packages,
                                                 const char *current_host) {
    size_t projected_installed;
    size_t projected_default_count = 0;
    size_t i;

    if (state == NULL || packages == NULL || text_is_empty(current_host)) {
        return CUP_ERR_INVALID_INPUT;
    }
    projected_installed = packages->total_count;

    /* Foreign-host records are outside the current scan but remain part of the final state. */
    for (i = 0; i < state->installed_count; ++i) {
        if (strcmp(state->installed[i].host_platform, current_host) != 0) {
            projected_installed++;
        }
    }
    for (i = 0; i < state->default_count; ++i) {
        if (strcmp(state->defaults[i].host_platform, current_host) != 0 ||
            package_list_contains(packages, &state->defaults[i])) {
            projected_default_count++;
        }
    }

    if (projected_installed > MAX_INSTALLED || projected_default_count > MAX_STATE_DEFAULTS) {
        fprintf(stderr,
                "Error: the complete projected state requires %zu installed and %zu default "
                "records, but the bounded model supports %d and %d; repair did not modify "
                "packages or state.\n",
                projected_installed,
                projected_default_count,
                MAX_INSTALLED,
                MAX_STATE_DEFAULTS);
        return CUP_ERR_STATE_FULL;
    }
    return CUP_OK;
}

static CupError repair_packages(RepairContext *context) {
    CupError err;
    int quarantined_any;

    err = package_scan(&context->packages, NULL);
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

    err = require_projected_state_capacity(
        &context->state, &context->packages, context->current_host);
    if (err != CUP_OK) {
        return err;
    }

    err = quarantine_invalid_packages(&context->packages, &quarantined_any);
    if (err != CUP_OK) {
        return err;
    }

    if (quarantined_any) {
        err = package_scan(&context->packages, NULL);
        if (err != CUP_OK) {
            return err;
        }

        err = require_complete_package_scan(&context->packages);
        if (err != CUP_OK) {
            return err;
        }

        err = require_projected_state_capacity(
            &context->state, &context->packages, context->current_host);
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

    err = state_save(
        &context->state,
        context->state_status == STATE_FILE_LOADED ? &context->state_identity : NULL,
        NULL);
    if (err != CUP_OK) {
        return err;
    }

    printf("Saved a valid state.txt.\n");
    return CUP_OK;
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

/* Repair is the only command allowed to recover an unusable cup.lock pathname. A wrong-kind
 * object is preserved as evidence before the canonical exclusive lock is created. Existing roots
 * are otherwise never marked, chmodded or initialized before that lock is held. */
static CupError acquire_repair_lock(RepairContext *context) {
    SystemPathKind root_kind;
    SystemPathKind lock_kind;
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char backup_path[MAX_PATH_LEN];

    err = layout_get_root(root_path, sizeof(root_path));
    if (err == CUP_OK) {
        err = system_get_path_kind(root_path, &root_kind);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (root_kind == SYSTEM_PATH_MISSING) {
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = layout_ensure_root();
        }
        if (err != CUP_OK) {
            return err;
        }
    } else if (root_kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) {
        err = system_get_path_kind(lock_path, &lock_kind);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (lock_kind != SYSTEM_PATH_MISSING && lock_kind != SYSTEM_PATH_REGULAR_FILE) {
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            /* No valid canonical lock can exist while cup.lock has the wrong kind. Revalidate the
             * selected root and active handoff immediately before this one exceptional
             * pre-lock mutation. */
            err = layout_root_snapshot_validate();
        }
        if (err == CUP_OK) {
            err = filesystem_backup_invalid(lock_path, backup_path, sizeof(backup_path));
        }
        if (err != CUP_OK) {
            return err;
        }
        printf("Preserved invalid lock path as '%s'.\n", backup_path);
    }

    err = system_lock_acquire(&context->lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) {
        err = layout_root_snapshot_validate();
    }
    if (err == CUP_OK) {
        err = layout_ensure_root();
    }
    if (err != CUP_OK) {
        if (context->lock.active) {
            system_lock_release(&context->lock);
        }
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
    }
    return err;
}

/* Recover durable transaction state before rebuilding derivable packages, state and wrappers;
 * any unresolved phase stops later mutations. */
CupError command_repair(void) {
    RepairContext context;
    CupError err;

    repair_context_init(&context);
    printf("==> Repairing cup...\n");

    err = platform_get_host(context.current_host, sizeof(context.current_host));
    if (err != CUP_OK) {
        return err;
    }

    err = acquire_repair_lock(&context);
    if (err != CUP_OK) {
        return err;
    }

    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = layout_ensure_runtime();
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = repair_load_state(&context);
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = repair_pending_transaction(&context);
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = repair_assets();
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = repair_packages(&context);
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = repair_save_state(&context);
    }
    if (err == CUP_OK) {
        WrapperPlan wrappers;

        wrapper_plan_init(&wrappers);
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = wrapper_plan_build(&wrappers, &context.state);
        }
        if (err == CUP_OK) {
            err = wrapper_plan_apply(&wrappers);
        }
        wrapper_plan_free(&wrappers);
        if (err == CUP_OK) {
            printf("Rebuilt managed wrappers.\n");
        }
    }
    if (err == CUP_OK) {
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = repair_cleanup_staging(&context);
        }
    }

    if (err == CUP_OK) {
        printf("Repair completed.\n");
    }

    system_lock_release(&context.lock);
    return err;
}
