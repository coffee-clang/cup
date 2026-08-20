/*
 * Runs the managed native helper copy used to complete a cup update after the parent process
 * releases the executable and lock.
 */

#include "cup_update_helper.h"

#include "constants.h"
#include "checksum.h"
#include "cup_assets.h"
#include "cup_update_journal.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define HELPER_LOCK_ATTEMPTS 600
#define HELPER_LOCK_DELAY_MS 100

typedef struct {
    const char *new_name;
    const char *old_name;
    const char *absent_name;
    char destination[MAX_PATH_LEN];
    int executable;
    int read_only;
} HelperAsset;

/* Fixed generation asset table. The helper replaces only the official cup assets described here. */
static CupError initialize_assets(HelperAsset *assets, size_t count) {
    if (assets == NULL || count != 6) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(assets, 0, sizeof(*assets) * count);
    assets[0].new_name = CUP_UPDATE_BINARY_NEW;
    assets[0].old_name = CUP_UPDATE_BINARY_OLD;
    assets[0].absent_name = CUP_UPDATE_BINARY_ABSENT;
    assets[0].executable = 1;
    if (layout_get_binary_path(assets[0].destination, sizeof(assets[0].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[1].new_name = CUP_UPDATE_UNINSTALL_NEW;
    assets[1].old_name = CUP_UPDATE_UNINSTALL_OLD;
    assets[1].absent_name = CUP_UPDATE_UNINSTALL_ABSENT;
    assets[1].executable = CUP_UNINSTALL_EXECUTABLE;
    assets[1].read_only = 1;
    if (layout_get_uninstall_path(assets[1].destination, sizeof(assets[1].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[2].new_name = CUP_UPDATE_PLATFORM_CHECKSUMS_NEW;
    assets[2].old_name = CUP_UPDATE_PLATFORM_CHECKSUMS_OLD;
    assets[2].absent_name = CUP_UPDATE_PLATFORM_CHECKSUMS_ABSENT;
    assets[2].read_only = 1;
    if (layout_get_platform_checksums_path(assets[2].destination,
                                           sizeof(assets[2].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[3].new_name = CUP_UPDATE_PACKAGES_NEW;
    assets[3].old_name = CUP_UPDATE_PACKAGES_OLD;
    assets[3].absent_name = CUP_UPDATE_PACKAGES_ABSENT;
    assets[3].read_only = 1;
    if (layout_get_package_catalog_path(assets[3].destination,
                                        sizeof(assets[3].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[4].new_name = CUP_UPDATE_INSTALL_POLICY_NEW;
    assets[4].old_name = CUP_UPDATE_INSTALL_POLICY_OLD;
    assets[4].absent_name = CUP_UPDATE_INSTALL_POLICY_ABSENT;
    assets[4].read_only = 1;
    if (layout_get_install_policy_path(assets[4].destination,
                                       sizeof(assets[4].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[5].new_name = CUP_UPDATE_COMMON_CHECKSUMS_NEW;
    assets[5].old_name = CUP_UPDATE_COMMON_CHECKSUMS_OLD;
    assets[5].absent_name = CUP_UPDATE_COMMON_CHECKSUMS_ABSENT;
    assets[5].read_only = 1;
    if (layout_get_common_checksums_path(assets[5].destination,
                                         sizeof(assets[5].destination)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    return CUP_OK;
}

static CupError validate_staged_assets(const char *staging,
                                       const HelperAsset *assets,
                                       size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        char source[MAX_PATH_LEN];
        SystemPathKind kind;

        if (path_join(source, sizeof(source), staging, assets[i].new_name) != CUP_OK ||
            system_get_path_kind(source, &kind) != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) {
            return CUP_ERR_VALIDATION;
        }
    }
    return CUP_OK;
}

/* Commit protocol. Backups are copies so every canonical destination remains present until its
 * atomically verified replacement is committed. */
static CupError create_absent_evidence(const char *path) {
    FILE *file = NULL;
    CupError err = system_create_file_exclusive(path, &file);
    int failed = 0;

    if (err != CUP_OK) {
        return err;
    }
    if (system_sync_file(file) != CUP_OK) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed || system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

static CupError backup_destinations(const char *staging, const HelperAsset *assets, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        char backup[MAX_PATH_LEN];
        char absent[MAX_PATH_LEN];
        SystemPathKind destination_kind;
        SystemPathKind backup_kind;
        SystemPathKind absent_kind;
        CupError err;

        if (path_join(backup, sizeof(backup), staging, assets[i].old_name) != CUP_OK ||
            path_join(absent, sizeof(absent), staging, assets[i].absent_name) != CUP_OK ||
            system_get_path_kind(assets[i].destination, &destination_kind) != CUP_OK ||
            system_get_path_kind(backup, &backup_kind) != CUP_OK ||
            system_get_path_kind(absent, &absent_kind) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (backup_kind != SYSTEM_PATH_MISSING || absent_kind != SYSTEM_PATH_MISSING) {
            return CUP_ERR_TRANSACTION;
        }
        if (destination_kind == SYSTEM_PATH_MISSING) {
            err = create_absent_evidence(absent);
        } else if (destination_kind == SYSTEM_PATH_REGULAR_FILE) {
            err = system_copy_file(assets[i].destination, backup);
        } else {
            return CUP_ERR_VALIDATION;
        }
        if (err != CUP_OK) {
            return err == CUP_ERR_COMMIT ? err : CUP_ERR_TRANSACTION;
        }
    }
    return CUP_OK;
}

static CupError install_staged_asset(const char *staging, const HelperAsset *asset) {
    char source[MAX_PATH_LEN];
    SystemPathKind destination_kind;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    if (path_join(source, sizeof(source), staging, asset->new_name) != CUP_OK ||
        system_get_path_kind(asset->destination, &destination_kind) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (destination_kind == SYSTEM_PATH_REGULAR_FILE) {
        if (system_set_read_only(asset->destination, 0) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
    } else if (destination_kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_VALIDATION;
    }

    err = system_replace_file(source, asset->destination, &commit_state);
    if (err != CUP_OK) {
        return commit_state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_TRANSACTION : CUP_ERR_COMMIT;
    }
    err = filesystem_apply_required_permissions(
        asset->destination, asset->executable, asset->read_only);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: could not apply permissions to cup update asset '%s'.\n",
                asset->destination);
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

static CupError install_supporting_assets(const char *staging,
                                          const HelperAsset *assets,
                                          size_t count) {
    size_t i;

    for (i = 1; i < count; ++i) {
        CupError err = install_staged_asset(staging, &assets[i]);

        if (err != CUP_OK) {
            return err;
        }
    }
    return CUP_OK;
}

static CupError commit_update(CupUpdateJournal *journal, const char *staging) {
    HelperAsset assets[6];
    char staged_binary[MAX_PATH_LEN];
    CupError err;

    err = initialize_assets(assets, sizeof(assets) / sizeof(assets[0]));
    if (err == CUP_OK) {
        err = validate_staged_assets(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    if (err == CUP_OK) {
        err = backup_destinations(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    /* COMMITTING is published only after every destination has durable rollback evidence. Until
     * this publication succeeds, canonical CUP assets are unchanged and scheduled recovery may
     * discard the staging tree without trying to restore it. */
    if (err == CUP_OK) {
        err = cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_COMMITTING, 0);
    }
    if (err == CUP_OK) {
        err = install_supporting_assets(staging, assets, sizeof(assets) / sizeof(assets[0]));
    }
    if (err == CUP_OK) {
        err = path_join(
            staged_binary, sizeof(staged_binary), staging, CUP_UPDATE_BINARY_NEW);
    }
    if (err == CUP_OK) {
        err = cup_update_write_generation_marker(staging, journal->version, staged_binary);
    }
    /* The executable is replaced last, after every supporting asset and the durable marker are in
     * place. Until that final replacement, the old executable remains present and the journal plus
     * backups describe a deterministic rollback. */
    if (err == CUP_OK) {
        err = install_staged_asset(staging, &assets[0]);
    }
    if (err == CUP_OK) {
        CupAssetsInspection inspection;

        err = cup_assets_inspect(&inspection);
        if (err == CUP_OK && !cup_assets_installed_is_valid(&inspection)) {
            err = CUP_ERR_VALIDATION;
        }
    }
    if (err == CUP_OK) {
        err = runtime_journal_clear_if_identity(&journal->file_identity);
    }
    if (err == CUP_OK) {
        if (filesystem_remove_tree(staging) != CUP_OK) {
            fprintf(stderr,
                    "Warning: cup was updated successfully, but stale update staging could not "
                    "be removed. Run 'cup repair'.\n");
        }
    }
    return err;
}

/* Persist one detached failure in transaction.txt before attempting deterministic recovery. */
static CupError record_helper_failure(CupUpdateJournal *journal,
                                      CupError error,
                                      int recover) {
    CupError err;
    CupUpdateRecoveryResult recovery_result = CUP_UPDATE_RECOVERY_NONE;

    if (journal == NULL || error == CUP_OK) {
        return error;
    }

    err = cup_update_journal_set_phase(journal, CUP_UPDATE_PHASE_FAILED, (int)error);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: the cup update failure could not be persisted; transaction evidence "
                "was preserved.\n");
        return err;
    }
    if (!recover) {
        return error;
    }

    err = cup_update_journal_recover(
        journal, CUP_UPDATE_RECOVER_REPLACE_BINARY, &recovery_result);
    if (err != CUP_OK) {
        return error;
    }
    return recovery_result == CUP_UPDATE_RECOVERY_FINALIZED ? CUP_OK : error;
}

static CupError acquire_helper_lock(SystemLock *lock) {
    char lock_path[MAX_PATH_LEN];
    int attempt;

    if (layout_get_lock_path(lock_path, sizeof(lock_path)) != CUP_OK) {
        return CUP_ERR_LOCK;
    }
    for (attempt = 0; attempt < HELPER_LOCK_ATTEMPTS; ++attempt) {
        CupError err = system_lock_acquire(lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) {
            err = layout_root_snapshot_validate();
            if (err == CUP_OK) {
                return CUP_OK;
            }
            system_lock_release(lock);
            return err;
        }
        if (err != CUP_ERR_LOCK) {
            return err;
        }
        err = system_sleep_milliseconds(HELPER_LOCK_DELAY_MS);
        if (err != CUP_OK) {
            return err;
        }
    }
    return CUP_ERR_LOCK;
}

/* Parent-side handoff. Ensure the canonical helper matches the running cup binary before the
 * parent releases control. */
static CupError helper_matches_binary(const char *binary, const char *helper, int *matches) {
    char binary_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char helper_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    SystemPathKind helper_kind;
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    err = system_get_path_kind(helper, &helper_kind);
    if (err != CUP_OK || helper_kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (helper_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_VALIDATION;
    }
    err = checksum_sha256_file(binary, binary_hash, sizeof(binary_hash));
    if (err != CUP_OK) {
        return err;
    }
    err = checksum_sha256_file(helper, helper_hash, sizeof(helper_hash));
    if (err != CUP_OK) {
        return err;
    }

    *matches = strcmp(binary_hash, helper_hash) == 0;
    return CUP_OK;
}

CupError cup_update_helper_prepare_from(const char *source_binary) {
    char helper[MAX_PATH_LEN];
    CupError err;
    int matches = 0;

    if (text_is_empty(source_binary)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_ensure_cup_assets();
    if (err == CUP_OK) {
        err = layout_get_cup_update_helper_path(helper, sizeof(helper));
    }
    if (err == CUP_OK) {
        err = helper_matches_binary(source_binary, helper, &matches);
    }
    if (err == CUP_OK && !matches) {
        err = system_copy_file(source_binary, helper);
    }
    if (err == CUP_OK) {
        err = system_set_executable(helper, 1);
    }
    if (err == CUP_OK) {
        err = helper_matches_binary(source_binary, helper, &matches);
    }
    if (err == CUP_OK && !matches) {
        err = CUP_ERR_VALIDATION;
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not prepare the native cup update helper.\n");
    }
    return err;
}

CupError cup_update_helper_prepare(void) {
    char binary[MAX_PATH_LEN];
    CupError err = layout_get_binary_path(binary, sizeof(binary));

    return err == CUP_OK ? cup_update_helper_prepare_from(binary) : err;
}

CupError cup_update_helper_start(const char *token) {
    char helper[MAX_PATH_LEN];

    if (text_is_empty(token) ||
        layout_get_cup_update_helper_path(helper, sizeof(helper)) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    return system_start_cup_update_helper(helper, token);
}

/* Detached helper execution. The parent handshake, token and global lock are all verified before
 * committing assets. */
CupError cup_update_helper_run(const char *token, const char *wait_value) {
    CupUpdateJournal journal;
    CupUpdateJournalStatus status;
    SystemLock lock = {0};
    char staging[MAX_PATH_LEN];
    CupError err;

    err = system_wait_for_parent_exit(wait_value);
    if (err != CUP_OK) {
        return err;
    }
    err = acquire_helper_lock(&lock);
    if (err != CUP_OK) {
        /* Without the global lock, the helper must not read or rewrite transaction authority that
         * repair or another process may already be changing. */
        return err;
    }

    err = cup_update_journal_load(&journal, &status);
    if (err != CUP_OK || status != CUP_UPDATE_JOURNAL_LOADED || strcmp(journal.token, token) != 0 ||
        journal.phase != CUP_UPDATE_PHASE_SCHEDULED) {
        system_lock_release(&lock);
        return CUP_ERR_TRANSACTION;
    }
    err = cup_update_journal_get_staging_path(&journal, staging, sizeof(staging));
    if (err != CUP_OK) {
        system_lock_release(&lock);
        return err;
    }

    err = commit_update(&journal, staging);
    if (err != CUP_OK && journal.phase != CUP_UPDATE_PHASE_SCHEDULED) {
        err = record_helper_failure(&journal, err, 1);
    }
    system_lock_release(&lock);
    return err;
}
