/*
 * Runs uninstall from a temporary native copy outside the managed root. The parent prepares all
 * persistent state and hands over exclusive authority; this helper waits for parent exit, detaches
 * the exact root, removes its contents without following links, then releases the handoff.
 */

#include "uninstall_helper.h"

#include "checksum.h"
#include "constants.h"
#include "exit_status.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"
#include "uninstall_journal.h"

#include <stdio.h>
#include <string.h>

static CupError build_helper_path(char *buffer,
                                  size_t size,
                                  const char *root,
                                  const char *token) {
    char parent[MAX_PATH_LEN];
    char name[MAX_METADATA_LINE_LEN];
    CupError err;

    if (buffer == NULL || size == 0 || text_is_empty(root) || text_is_empty(token)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_parent(parent, sizeof(parent), root);
#if defined(_WIN32)
    if (err == CUP_OK) {
        err = text_format(name, sizeof(name), ".cup-uninstall-helper-%s.exe", token);
    }
#else
    if (err == CUP_OK) {
        err = text_format(name, sizeof(name), ".cup-uninstall-helper-%s", token);
    }
#endif
    if (err != CUP_OK || !path_is_safe_segment(name)) {
        return err == CUP_OK ? CUP_ERR_INVALID_INPUT : err;
    }
    return path_join(buffer, size, parent, name);
}

static CupError build_detached_path(char *buffer,
                                    size_t size,
                                    const char *root,
                                    const UninstallJournal *journal) {
    char parent[MAX_PATH_LEN];
    CupError err;

    if (buffer == NULL || size == 0 || text_is_empty(root) || journal == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_parent(parent, sizeof(parent), root);
    return err == CUP_OK ? path_join(buffer, size, parent, journal->temporary_name) : err;
}

static CupError files_match(const char *left, const char *right, int *matches) {
    char left_digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char right_digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;
    err = checksum_sha256_file(left, left_digest, sizeof(left_digest));
    if (err == CUP_OK) {
        err = checksum_sha256_file(right, right_digest, sizeof(right_digest));
    }
    if (err == CUP_OK) {
        *matches = strcmp(left_digest, right_digest) == 0;
    }
    return err;
}

static CupError prepare_helper(char *helper,
                               size_t helper_size,
                               const char *root,
                               const char *token) {
    char running[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;
    int matches = 0;

    err = build_helper_path(helper, helper_size, root, token);
    if (err == CUP_OK) {
        err = system_get_path_kind(helper, &kind);
    }
    if (err != CUP_OK || kind != SYSTEM_PATH_MISSING) {
        return err == CUP_OK ? CUP_ERR_TEMPORARY : err;
    }
    err = system_get_executable_path(running, sizeof(running));
    if (err == CUP_OK) {
        err = system_copy_file(running, helper);
        if (err == CUP_ERR_COMMIT) {
            /* The helper is disposable. Visibility plus byte-for-byte validation is sufficient;
             * its publication is not a canonical managed-root commit boundary. */
            err = CUP_OK;
        }
    }
    if (err == CUP_OK) {
        err = system_set_executable(helper, 1);
    }
    if (err == CUP_OK) {
        err = files_match(running, helper, &matches);
    }
    if (err == CUP_OK && !matches) {
        err = CUP_ERR_VALIDATION;
    }
    return err;
}

CupError uninstall_helper_remove_stale(const char *root,
                                       const char *token,
                                       const SystemLock *lock) {
    char helper[MAX_PATH_LEN];
    SystemPathIdentity identity;
    SystemPathKind kind;
    CupError err;

    if (text_is_empty(root) || text_is_empty(token) || lock == NULL || !lock->active ||
        lock->mode != SYSTEM_LOCK_EXCLUSIVE) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = build_helper_path(helper, sizeof(helper), root, token);
    if (err == CUP_OK) {
        err = system_get_path_kind(helper, &kind);
    }
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }
    err = system_get_path_identity(helper, &identity);
    if (err != CUP_OK || !identity.valid || identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        return err == CUP_OK ? CUP_ERR_TRANSACTION : err;
    }
    return system_remove_file_if_identity(helper, &identity);
}

CupError uninstall_helper_start(const char *root,
                                const char *detached_root,
                                const char *token,
                                SystemLock *lock) {
    char helper[MAX_PATH_LEN];
    CupError err;

    if (text_is_empty(root) || text_is_empty(detached_root) || text_is_empty(token) ||
        lock == NULL || !lock->active || lock->mode != SYSTEM_LOCK_EXCLUSIVE) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = prepare_helper(helper, sizeof(helper), root, token);
    if (err == CUP_OK) {
        err = system_start_uninstall_helper(helper, root, detached_root, token, lock);
    }
    if (err != CUP_OK) {
        CupError cleanup_err = uninstall_helper_remove_stale(root, token, lock);

        if (cleanup_err != CUP_OK) {
            return cleanup_err;
        }
    }
    return err;
}

static CupError validate_handoff(const char *root,
                                 const char *detached_root,
                                 const char *token,
                                 SystemPathIdentity *root_identity,
                                 UninstallJournal *journal) {
    UninstallJournalStatus status;
    char expected_detached[MAX_PATH_LEN];
    CupError err;

    err = layout_validate_root_at(root, root_identity);
    if (err == CUP_OK) {
        err = uninstall_journal_load_at(root, journal, &status);
    }
    if (err != CUP_OK || status != UNINSTALL_JOURNAL_LOADED ||
        journal->phase != UNINSTALL_PHASE_SCHEDULED ||
        journal->stage != UNINSTALL_STAGE_HANDOFF || journal->error_code != 0 ||
        strcmp(journal->token, token) != 0) {
        return CUP_ERR_TRANSACTION;
    }
    err = build_detached_path(expected_detached, sizeof(expected_detached), root, journal);
    if (err != CUP_OK || strcmp(expected_detached, detached_root) != 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError remove_detached_root(const char *detached_root,
                                     const UninstallJournal *journal) {
    char transaction[MAX_PATH_LEN];
    SystemPathIdentity detached_identity;
    CupError err;

    if (text_is_empty(detached_root) || journal == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = system_get_path_identity(detached_root, &detached_identity);
    if (err != CUP_OK || !detached_identity.valid ||
        detached_identity.kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_TRANSACTION;
    }
    /* Some filesystems may refresh a directory ID across rename. The identity-bound move already
     * proved destination continuity; retain the post-move identity for all destructive cleanup. */

    err = layout_build_transaction_path(transaction, sizeof(transaction), detached_root);
    if (err == CUP_OK) {
        err = system_remove_tree_contents(detached_root, path_last_segment(transaction), NULL);
    }
    if (err == CUP_OK) {
        err = system_remove_file_if_identity(transaction, &journal->file_identity);
    }
    if (err == CUP_OK) {
        err = system_remove_path_if_identity(detached_root, &detached_identity, NULL);
    }
    return err;
}

CupError uninstall_helper_run(const char *root,
                              const char *detached_root,
                              const char *token,
                              const char *parent_signal_value,
                              const char *authority_value) {
    SystemHandoff handoff = {0};
    SystemPathIdentity root_identity;
    UninstallJournal journal;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    char helper[MAX_PATH_LEN];
    CupError err;

    memset(&root_identity, 0, sizeof(root_identity));
    uninstall_journal_init(&journal);
    if (text_is_empty(root) || text_is_empty(detached_root) || text_is_empty(token) ||
        text_is_empty(parent_signal_value) || text_is_empty(authority_value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Remove the temporary helper name as soon as its exact reserved identity is proven. The
     * mapped process remains executable on both supported backends, so no third cleanup process is
     * needed and an unexpected later failure cannot strand the helper file. */
    err = build_helper_path(helper, sizeof(helper), root, token);
    if (err != CUP_OK) {
        return err;
    }
    /* The backend verifies that this reserved helper path names the current executable by native
     * identity. Avoid textual path equality: Windows path spelling and case are not identity. */
    err = system_unlink_running_executable(helper);
    if (err != CUP_OK) {
        return err;
    }

    err = system_handoff_accept(&handoff, parent_signal_value, authority_value);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_handoff(root, detached_root, token, &root_identity, &journal);
    if (err != CUP_OK) {
        system_handoff_release(&handoff);
        return err;
    }

    err = uninstall_journal_set_at(root,
                                   &journal,
                                   UNINSTALL_PHASE_DETACHING,
                                   UNINSTALL_STAGE_DETACH,
                                   0);
    if (err != CUP_OK) {
        system_handoff_release(&handoff);
        return err;
    }

    err = system_move_path_retry(root, detached_root, &root_identity, &commit_state);
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
            CupError journal_err = uninstall_journal_set_at(root,
                                                            &journal,
                                                            UNINSTALL_PHASE_FAILED,
                                                            UNINSTALL_STAGE_DETACH,
                                                            CUP_STATUS_OPERATION);

            system_handoff_release(&handoff);
            return journal_err == CUP_OK || journal_err == CUP_ERR_COMMIT ? err : journal_err;
        }

        /* The namespace move happened but its durable/identity proof is incomplete. Do not touch
         * the destination through an unproven pathname; the moved detach journal is the recovery
         * evidence for repair. */
        system_handoff_release(&handoff);
        return CUP_ERR_COMMIT;
    }

    /* Once a durable namespace move is proven, the detached journal remains the last ownership
     * evidence until all managed payload is gone. A cleanup failure intentionally leaves it. */
    {
        UninstallJournalStatus status;

        err = uninstall_journal_load_at(detached_root, &journal, &status);
        if (err == CUP_OK &&
            (status != UNINSTALL_JOURNAL_LOADED ||
             journal.phase != UNINSTALL_PHASE_DETACHING ||
             journal.stage != UNINSTALL_STAGE_DETACH || journal.error_code != 0 ||
             strcmp(journal.token, token) != 0)) {
            err = CUP_ERR_TRANSACTION;
        }
    }
    if (err == CUP_OK) {
        err = remove_detached_root(detached_root, &journal);
    }
    system_handoff_release(&handoff);
    return err;
}
