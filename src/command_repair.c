/*
 * Reconciles interrupted operations, packages, state and derived runtime views before
 * repairing the installed cup generation and live catalog.
 */

#include "commands.h"
#include "download.h"

#include "checksum.h"
#include "generation.h"
#include "release_metadata.h"
#include "tool_preferences.h"

#include "package_selector.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "interrupt.h"
#include "package_catalog.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "update_journal.h"
#include "runtime_journal.h"
#include "runtime_recovery.h"
#include "uninstall_helper.h"
#include "uninstall_journal.h"
#include "text.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CUP_VERSION_OFFICIAL
static CupError restore_asset_backup(const char *backup_path, const char *destination) {
    SystemCommitState restore_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err = system_move_path(backup_path, destination, &restore_state);
    return err == CUP_OK ? CUP_OK : CUP_ERR_ROLLBACK;
}

static CupError commit_asset(const char *staged_path,
                             const char *destination,
                             const char *description) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char backup_path[MAX_PATH_LEN];
    int has_backup = 0;
    int destination_exists;

    err = system_path_exists(destination, &destination_exists);
    if (err != CUP_OK) return err;
    if (destination_exists) {
        err = filesystem_backup_invalid(destination, backup_path, sizeof(backup_path));
        if (err != CUP_OK) return err;
        has_backup = 1;
    }

    err = system_replace_file(staged_path, destination, &commit_state);
    if (err == CUP_OK) {
        if (has_backup) printf("Preserved invalid %s as '%s'.\n", description, backup_path);
        return CUP_OK;
    }
    if (commit_state == SYSTEM_COMMIT_APPLIED) {
        if (has_backup) printf("Preserved invalid %s as '%s'.\n", description, backup_path);
        fprintf(stderr,
                "Error: repaired %s may already be published, but its durability could not be "
                "confirmed. Run 'cup doctor' before retrying.\n",
                description);
        return CUP_ERR_COMMIT;
    }
    if (has_backup && restore_asset_backup(backup_path, destination) != CUP_OK) {
        fprintf(stderr,
                "Error: replacement failed and the previous %s could not be restored.\n",
                description);
        return CUP_ERR_ROLLBACK;
    }
    return err;
}

#endif

#if CUP_VERSION_OFFICIAL
static CupError create_repair_temp(char *path, size_t path_size) {
    char staging_dir[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;

    err = layout_get_staging_dir(staging_dir, sizeof(staging_dir));
    if (err == CUP_OK) err = system_create_temp_file(staging_dir, "repair", path, path_size, &file);
    if (err != CUP_OK) return CUP_ERR_TEMPORARY;
    if (fclose(file) != 0) {
        system_remove_file(path);
        return CUP_ERR_TEMPORARY;
    }
    return CUP_OK;
}

static CupError release_base_url(char *base, size_t size) {
    CupError err = download_copy_release_base_override(base, size);
    if (err == CUP_ERR_NOT_AVAILABLE) {
        return text_format(base, size, CUP_RELEASE_VERSIONED_URL_TEMPLATE, CUP_VERSION_BASE);
    }
    return err;
}

static CupError download_release_asset(const char *name, char *path, size_t path_size) {
    char base[MAX_CATALOG_URL_LEN];
    char url[MAX_CATALOG_URL_LEN];
    CupError err;

    if (text_is_empty(name)) return CUP_ERR_INVALID_INPUT;
    err = release_base_url(base, sizeof(base));
    if (err == CUP_OK) err = text_format(url, sizeof(url), "%s/%s", base, name);
    if (err == CUP_OK) err = create_repair_temp(path, path_size);
    if (err == CUP_OK) err = download_file(url, path, DOWNLOAD_VALIDATE_METADATA);
    if (err != CUP_OK && path != NULL && path[0] != '\0') system_remove_file(path);
    return err;
}
#endif

static CupError file_matches_digest(const char *path, const char *expected, int *matches) {
    char digest[65];
    SystemPathKind kind;
    CupError err;

    if (path == NULL || expected == NULL || matches == NULL) return CUP_ERR_INVALID_INPUT;
    *matches = 0;
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) return err;
    if (kind != SYSTEM_PATH_REGULAR_FILE) return CUP_OK;
    err = checksum_sha256_file(path, digest, sizeof(digest));
    if (err == CUP_OK) *matches = strcmp(digest, expected) == 0;
    return err;
}

static CupError validate_release_for_current_binary(const ReleaseMetadata *metadata) {
    GenerationAssetSpec binary;
    const ReleaseAsset *asset;
    int matches;
    CupError err;

    if (metadata == NULL || generation_validate_manifest(metadata) != CUP_OK ||
        strcmp(metadata->version, CUP_VERSION_BASE) != 0) {
        return CUP_ERR_VALIDATION;
    }
    err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &binary);
    if (err != CUP_OK) return err;
    asset = generation_manifest_asset(metadata, CUP_GENERATION_ASSET_BINARY);
    if (asset == NULL) return CUP_ERR_VALIDATION;
    err = file_matches_digest(binary.destination, asset->sha256, &matches);
    if (err != CUP_OK) return err;
    if (!matches) {
        fprintf(stderr,
                "Error: the installed cup executable does not match the same-version release "
                "manifest. Repair preserved it unchanged; run the official installer.\n");
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError ensure_binary_permissions(void) {
    GenerationAssetSpec binary;
    int executable;
    CupError err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &binary);
    if (err != CUP_OK) return err;
    err = system_is_executable(binary.destination, &executable);
    if (err != CUP_OK) return err;
    if (executable) return CUP_OK;
    err = system_set_executable(binary.destination, 1);
    if (err == CUP_OK) printf("Restored executable permissions on the installed cup executable.\n");
    return err;
}

static CupError ensure_legal_asset(const ReleaseMetadata *metadata, GenerationAssetId id) {
    GenerationAssetSpec spec;
    const ReleaseAsset *asset;
    int matches;
    int read_only;
    CupError err;

    err = generation_asset_spec(id, &spec);
    if (err != CUP_OK) return err;
    asset = generation_manifest_asset(metadata, id);
    if (asset == NULL) return CUP_ERR_VALIDATION;
    err = file_matches_digest(spec.destination, asset->sha256, &matches);
    if (err != CUP_OK) return err;
    if (matches) {
        err = system_is_read_only(spec.destination, &read_only);
        if (err != CUP_OK) return err;
        return read_only ? CUP_OK : system_set_read_only(spec.destination, 1);
    }

#if !CUP_VERSION_OFFICIAL
    fprintf(stderr,
            "Error: installed generation asset '%s' is missing or invalid; a development build "
            "does not download official repair assets.\n",
            spec.release_name);
    return CUP_ERR_NOT_AVAILABLE;
#else
    {
        char staged[MAX_PATH_LEN] = "";
        int staged_matches;
        err = download_release_asset(spec.release_name, staged, sizeof(staged));
        if (err == CUP_OK) err = file_matches_digest(staged, asset->sha256, &staged_matches);
        if (err == CUP_OK && !staged_matches) err = CUP_ERR_VALIDATION;
        if (err == CUP_OK) err = system_set_read_only(staged, 1);
        if (err == CUP_OK) err = commit_asset(staged, spec.destination, spec.release_name);
        if (err != CUP_OK && staged[0] != '\0') system_remove_file(staged);
        if (err == CUP_OK) printf("Restored generation asset '%s'.\n", spec.release_name);
        return err;
    }
#endif
}

static CupError repair_generation(ReleaseMetadata *trusted, int *trusted_valid) {
    GenerationInspection inspection;
    GenerationAssetSpec release_spec;
    CupError err;
    int manifest_needs_commit = 0;
#if CUP_VERSION_OFFICIAL
    char staged_release[MAX_PATH_LEN] = "";
#endif

    if (trusted == NULL || trusted_valid == NULL) return CUP_ERR_INVALID_INPUT;
    *trusted_valid = 0;
    release_metadata_free(trusted);
    release_metadata_init(trusted);

    err = generation_inspect(&inspection);
    if (err != CUP_OK) return err;
    if (!generation_has_installed_assets(&inspection)) {
#if CUP_VERSION_OFFICIAL
        fprintf(stderr,
                "Error: the installed cup generation is missing. Run the official installer.\n");
        return CUP_ERR_NOT_INSTALLED;
#else
        return CUP_OK;
#endif
    }

    err = generation_asset_spec(CUP_GENERATION_ASSET_RELEASE, &release_spec);
    if (err != CUP_OK) return err;
    err = release_metadata_load(release_spec.destination, trusted);
    if (err == CUP_OK) err = validate_release_for_current_binary(trusted);
    if (err != CUP_OK) {
        release_metadata_free(trusted);
        release_metadata_init(trusted);
#if !CUP_VERSION_OFFICIAL
        fprintf(stderr,
                "Error: installed generation metadata is missing or invalid; a development "
                "build preserves it for reinstall.\n");
        return CUP_ERR_NOT_AVAILABLE;
#else
        err = download_release_asset(CUP_RELEASE_METADATA_FILENAME,
                                     staged_release,
                                     sizeof(staged_release));
        if (err == CUP_OK) err = release_metadata_load(staged_release, trusted);
        if (err == CUP_OK) err = validate_release_for_current_binary(trusted);
        if (err != CUP_OK) {
            if (staged_release[0] != '\0') system_remove_file(staged_release);
            release_metadata_free(trusted);
            release_metadata_init(trusted);
            return err;
        }
        manifest_needs_commit = 1;
#endif
    }

    err = ensure_binary_permissions();
    if (err == CUP_OK) err = ensure_legal_asset(trusted, CUP_GENERATION_ASSET_LICENSE);
    if (err == CUP_OK) err = ensure_legal_asset(trusted, CUP_GENERATION_ASSET_NOTICES);
#if CUP_VERSION_OFFICIAL
    if (err == CUP_OK && manifest_needs_commit) {
        err = system_set_read_only(staged_release, 1);
        if (err == CUP_OK) {
            err = commit_asset(staged_release, release_spec.destination, "release metadata");
        }
        if (err != CUP_OK && staged_release[0] != '\0') system_remove_file(staged_release);
        if (err == CUP_OK) printf("Restored same-version release metadata.\n");
    } else if (manifest_needs_commit && staged_release[0] != '\0') {
        system_remove_file(staged_release);
    }
#endif
    if (err == CUP_OK && !manifest_needs_commit) {
        int read_only;
        err = system_is_read_only(release_spec.destination, &read_only);
        if (err == CUP_OK && !read_only) err = system_set_read_only(release_spec.destination, 1);
    }
    if (err == CUP_OK) {
        GenerationInspection final_inspection;
        err = generation_inspect(&final_inspection);
        if (err == CUP_OK && !generation_installed_is_valid(&final_inspection)) {
            err = CUP_ERR_VALIDATION;
        }
    }
    if (err == CUP_OK) *trusted_valid = 1;
    return err;
}

static CupError preserve_invalid_path(const char *path, const char *description) {
    SystemPathKind kind;
    char backup[MAX_PATH_LEN];
    CupError err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) return err;
    if (kind == SYSTEM_PATH_MISSING) return CUP_OK;
    err = filesystem_backup_invalid(path, backup, sizeof(backup));
    if (err == CUP_OK) printf("Preserved invalid %s as '%s'.\n", description, backup);
    return err;
}

static CupError repair_preferences(void) {
    ToolPreferences preferences;
    char path[MAX_PATH_LEN];
    CupError err;

    tool_preferences_init(&preferences);
    err = tool_preferences_load(&preferences, NULL);
    if (err == CUP_OK) return CUP_OK;
    if (err == CUP_ERR_FILESYSTEM || err == CUP_ERR_TEMPORARY) return err;
    err = layout_get_preferences_path(path, sizeof(path));
    if (err != CUP_OK) return err;
    return preserve_invalid_path(path, "preferences");
}

static CupError repair_catalog(const ReleaseMetadata *trusted, int trusted_valid) {
    PackageCatalog catalog;
    CupError err;
    char path[MAX_PATH_LEN];

    package_catalog_init(&catalog);
    err = package_catalog_load_installed(&catalog);
    package_catalog_free(&catalog);
    if (err == CUP_OK) return CUP_OK;
    if (err == CUP_ERR_NOT_AVAILABLE) {
        fprintf(stderr,
                "Error: installed catalog uses a newer unsupported format; repair preserved it "
                "unchanged.\n");
        return err;
    }
    if (err != CUP_ERR_CATALOG && err != CUP_ERR_FILESYSTEM) return err;
    err = layout_get_package_catalog_path(path, sizeof(path));
    if (err != CUP_OK) return err;
    err = preserve_invalid_path(path, "catalog");
    if (err != CUP_OK) return err;

#if !CUP_VERSION_OFFICIAL
    (void)trusted;
    (void)trusted_valid;
    fprintf(stderr,
            "Error: development catalog is unavailable; place a published cup-components "
            "snapshot at './config/catalog.cfg' and run 'cup update catalog'.\n");
    return CUP_ERR_CATALOG;
#else
    {
        const ReleaseAsset *asset;
        char staged[MAX_PATH_LEN] = "";
        int matches;
        if (!trusted_valid || trusted == NULL) return CUP_ERR_VALIDATION;
        asset = release_metadata_find_asset(trusted, CUP_CATALOG_FILENAME);
        if (asset == NULL) return CUP_ERR_VALIDATION;
        err = download_release_asset(CUP_CATALOG_FILENAME, staged, sizeof(staged));
        if (err == CUP_OK) err = file_matches_digest(staged, asset->sha256, &matches);
        if (err == CUP_OK && !matches) err = CUP_ERR_VALIDATION;
        if (err == CUP_OK) {
            package_catalog_init(&catalog);
            err = package_catalog_load_path(&catalog, staged);
            package_catalog_free(&catalog);
        }
        if (err == CUP_OK) err = commit_asset(staged, path, "catalog");
        if (err != CUP_OK && staged[0] != '\0') system_remove_file(staged);
        if (err == CUP_OK) printf("Restored package catalog from the same cup release.\n");
        return err;
    }
#endif
}

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

        {
            CupError err = package_identity_format_selector(package, selector, sizeof(selector));
            if (err != CUP_OK) {
                return err;
            }
        }

        if (state_find_installed(state, package) == -1) {
            CupError err = state_add_installed(state, package);
            if (err != CUP_OK) {
                return err;
            }

            printf("Prepared state repair: adopt valid package '%s:%s'.\n", package->component, selector);
            *state_changed = 1;
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
    CupError err;
    int changed = 0;

    if (state == NULL || packages == NULL || state_changed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = remove_stale_installed_entries(state, packages, current_host, &changed);
    if (err == CUP_OK) {
        err = adopt_scanned_packages(state, packages, &changed);
    }
    if (err == CUP_OK) {
        err = remove_stale_defaults(state, current_host, &changed);
    }
    if (err == CUP_OK) {
        err = state_validate(state, NULL);
    }
    if (err != CUP_OK) {
        return err;
    }

    *state_changed = *state_changed || changed;
    return CUP_OK;
}


/* Ordered repair command. */
typedef struct {
    SystemLock lock;
    CupState state;
    StateFileStatus state_status;
    SystemPathIdentity state_identity;
    PackageList packages;
    UpdateJournal update_journal;
    UninstallJournal uninstall_journal;
    RuntimeJournalKind journal_kind;
    char current_host[MAX_PLATFORM_LEN];
    int state_changed;
    int state_invalid;
    int preserve_staging;
    ReleaseMetadata trusted_release;
    int trusted_release_valid;
} RepairContext;

static void repair_context_init(RepairContext *context) {
    memset(context, 0, sizeof(*context));
    state_init(&context->state);
    package_list_init(&context->packages);
    update_journal_init(&context->update_journal);
    uninstall_journal_init(&context->uninstall_journal);
    release_metadata_init(&context->trusted_release);
    context->journal_kind = RUNTIME_JOURNAL_MISSING;
}

static CupError detect_and_recover_transaction(RepairContext *context) {
    CupError err;

    err = runtime_recover_pending(&context->lock, &context->journal_kind);
    if (err != CUP_OK) {
        context->preserve_staging = 1;
    } else {
        context->journal_kind = RUNTIME_JOURNAL_MISSING;
    }
    return err;
}

static CupError scan_packages(RepairContext *context) {
    CupError err = package_scan(&context->packages, NULL);
    if (err != CUP_OK) return err;
    if (!context->packages.complete) {
        fprintf(stderr,
                "Error: package scan issue inventory is incomplete; repair did not mutate "
                "packages or state.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }
    return CUP_OK;
}

static CupError load_state_for_reconciliation(RepairContext *context) {
    CupError err;
    char state_path[MAX_PATH_LEN];
    SystemPathKind kind;

    err = state_load(&context->state,
                     &context->state_status,
                     &context->state_identity,
                     NULL);
    if (err == CUP_OK && context->state_status == STATE_FILE_LOADED &&
        state_validate(&context->state, NULL) == CUP_OK) {
        return CUP_OK;
    }
    if (err == CUP_OK && context->state_status == STATE_FILE_MISSING) {
        context->state_changed = 1;
        return CUP_OK;
    }
    if (err != CUP_ERR_STATE_LOAD && err != CUP_ERR_STATE_FULL && err != CUP_ERR_FILESYSTEM) {
        return err;
    }

    err = layout_get_state_path(state_path, sizeof(state_path));
    if (err != CUP_OK) return err;
    err = system_get_path_kind(state_path, &kind);
    if (err != CUP_OK) return err;
    if (kind == SYSTEM_PATH_MISSING) {
        state_free(&context->state);
        state_init(&context->state);
        context->state_status = STATE_FILE_MISSING;
        context->state_changed = 1;
        return CUP_OK;
    }
    if (!context->state_identity.valid && kind == SYSTEM_PATH_REGULAR_FILE) {
        err = system_get_path_identity(state_path, &context->state_identity);
        if (err != CUP_OK) return err;
    }
    state_free(&context->state);
    state_init(&context->state);
    context->state_invalid = 1;
    context->state_changed = 1;
    return CUP_OK;
}

static CupError preflight_reconciled_state(RepairContext *context) {
    size_t persistent_size;
    CupError err = reconcile_state(
        &context->state, &context->packages, context->current_host, &context->state_changed);
    if (err == CUP_OK) err = state_measure_persistent(&context->state, &persistent_size);
    if (err == CUP_ERR_STATE_FULL) {
        fprintf(stderr,
                "Error: complete reconstructed state exceeds the 4 MiB state budget; repair "
                "preserved package and state evidence without mutation.\n");
    }
    return err;
}

static CupError quarantine_invalid_packages(PackageList *packages) {
    size_t i;
    for (i = 0; i < packages->issue_count; ++i) {
        const PackageIssue *issue = &packages->issues[i];
        if (issue->can_quarantine) {
            char recovery_path[MAX_PATH_LEN];
            CupError err = package_quarantine(issue, recovery_path, sizeof(recovery_path));
            if (err != CUP_OK) return err;
            printf("Quarantined invalid package '%s' as '%s'.\n", issue->path, recovery_path);
        } else {
            printf("Warning: package path '%s' was left unchanged: %s.\n",
                   issue->path,
                   package_issue_reason_name(issue->reason));
        }
    }
    return CUP_OK;
}

static CupError preserve_invalid_state(RepairContext *context) {
    char path[MAX_PATH_LEN];
    char backup[MAX_PATH_LEN];
    CupError err;

    if (!context->state_invalid) return CUP_OK;
    err = layout_get_state_path(path, sizeof(path));
    if (err != CUP_OK) return err;
    if (context->state_identity.valid) {
        err = filesystem_backup_invalid_if_identity(
            path, &context->state_identity, backup, sizeof(backup));
    } else {
        err = filesystem_backup_invalid(path, backup, sizeof(backup));
    }
    if (err != CUP_OK) return err;
    printf("Preserved invalid state as '%s'.\n", backup);
    memset(&context->state_identity, 0, sizeof(context->state_identity));
    context->state_status = STATE_FILE_MISSING;
    context->state_invalid = 0;
    return CUP_OK;
}

static CupError save_reconciled_state(RepairContext *context) {
    CupError err;
    if (context->state_status == STATE_FILE_LOADED && !context->state_changed) return CUP_OK;
    err = state_save(&context->state,
                     context->state_status == STATE_FILE_LOADED ? &context->state_identity : NULL,
                     NULL);
    if (err == CUP_ERR_COMMIT) {
        fprintf(stderr,
                "Error: repaired state.txt may already be saved, but durability is uncertain. "
                "Run 'cup doctor' before retrying.\n");
    }
    if (err == CUP_OK) printf("Saved a valid state.txt.\n");
    return err;
}

static CupError rebuild_wrappers(const CupState *state) {
    WrapperPlan wrappers;
    CupError err;
    wrapper_plan_init(&wrappers);
    err = wrapper_plan_build(&wrappers, state);
    if (err == CUP_OK) err = wrapper_plan_apply(&wrappers);
    wrapper_plan_free(&wrappers);
    if (err == CUP_OK) printf("Rebuilt managed wrappers.\n");
    return err;
}

static CupError repair_cleanup_staging(const RepairContext *context) {
    char staging_dir[MAX_PATH_LEN];
    char transaction_path[MAX_PATH_LEN];
    if (context->preserve_staging) return CUP_OK;
    if (layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK ||
        layout_get_transaction_path(transaction_path, sizeof(transaction_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    return filesystem_clear_directory(staging_dir, transaction_path);
}

/* Only repair may recover an unusable cup.lock path. Preserve a wrong-kind object first;
 * otherwise existing roots are never modified before the canonical lock is held. */
static CupError acquire_repair_lock(RepairContext *context) {
    SystemPathKind root_kind;
    SystemPathKind lock_kind;
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char backup_path[MAX_PATH_LEN];

    err = layout_get_root(root_path, sizeof(root_path));
    if (err == CUP_OK) err = system_get_path_kind(root_path, &root_kind);
    if (err != CUP_OK) return err;
    if (root_kind == SYSTEM_PATH_MISSING) {
        fprintf(stderr, "Error: cup runtime is not installed.\n");
        return CUP_ERR_NOT_INSTALLED;
    }
    if (root_kind != SYSTEM_PATH_DIRECTORY) return CUP_ERR_FILESYSTEM;

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) err = system_get_path_kind(lock_path, &lock_kind);
    if (err != CUP_OK) return err;
    if (lock_kind != SYSTEM_PATH_MISSING && lock_kind != SYSTEM_PATH_REGULAR_FILE) {
        err = interrupt_safe_point();
        if (err == CUP_OK) err = layout_root_snapshot_validate();
        if (err == CUP_OK) err = filesystem_backup_invalid(lock_path, backup_path, sizeof(backup_path));
        if (err != CUP_OK) return err;
        printf("Preserved invalid lock path as '%s'.\n", backup_path);
    }

    err = system_lock_acquire(&context->lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) err = layout_root_snapshot_validate();
    if (err == CUP_OK) err = layout_ensure_root();
    if (err != CUP_OK) {
        if (context->lock.active) system_lock_release(&context->lock);
        if (err == CUP_ERR_LOCK) fprintf(stderr, "Error: another cup operation is currently running.\n");
    }
    return err;
}

CupError command_repair(void) {
    RepairContext context;
    CupError err;

    repair_context_init(&context);
    printf("==> Repairing cup...\n");

    err = platform_get_host(context.current_host, sizeof(context.current_host));
    if (err != CUP_OK) goto done;
    err = acquire_repair_lock(&context);
    if (err != CUP_OK) goto done;
    err = interrupt_safe_point();
    if (err == CUP_OK) err = layout_ensure_runtime();

    /* Transaction evidence is authoritative and must be resolved before any reconstruction. */
    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) err = detect_and_recover_transaction(&context);

    /* Scan and compute the complete reconstructed state before the first package/state mutation. */
    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) err = scan_packages(&context);
    if (err == CUP_OK) err = load_state_for_reconciliation(&context);
    if (err == CUP_OK) err = preflight_reconciled_state(&context);
    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) err = quarantine_invalid_packages(&context.packages);
    if (err == CUP_OK) err = preserve_invalid_state(&context);
    if (err == CUP_OK) err = save_reconciled_state(&context);

    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) err = repair_preferences();
    if (err == CUP_OK) err = rebuild_wrappers(&context.state);
    if (err == CUP_OK) err = repair_cleanup_staging(&context);

    /* Official generation and live catalog are independent of package/state authority. */
    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = repair_generation(&context.trusted_release, &context.trusted_release_valid);
    }
    if (err == CUP_OK) err = repair_catalog(&context.trusted_release, context.trusted_release_valid);

    if (err == CUP_OK) printf("Repair completed.\n");

done:
    if (context.lock.active) system_lock_release(&context.lock);
    release_metadata_free(&context.trusted_release);
    state_free(&context.state);
    package_list_free(&context.packages);
    return err;
}
