/* Recover one pending shared runtime transaction from its owner-specific commit evidence. */

#include "runtime_recovery.h"

#include "layout.h"
#include "package_transaction.h"
#include "state.h"
#include "uninstall_helper.h"
#include "uninstall_journal.h"
#include "update_journal.h"

#include <stdio.h>

CupError runtime_recover_pending(const SystemLock *lock, RuntimeJournalKind *recovered_kind) {
    RuntimeJournalKind kind = RUNTIME_JOURNAL_MISSING;
    CupError err;

    if (recovered_kind != NULL) *recovered_kind = RUNTIME_JOURNAL_MISSING;
    if (lock == NULL || !lock->active || lock->mode != SYSTEM_LOCK_EXCLUSIVE) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = runtime_journal_detect(&kind);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: transaction.txt is invalid; recovery preserved it unchanged.\n");
        return CUP_ERR_TRANSACTION;
    }
    if (kind == RUNTIME_JOURNAL_MISSING) return CUP_OK;
    if (recovered_kind != NULL) *recovered_kind = kind;

    if (kind == RUNTIME_JOURNAL_PACKAGE) {
        PackageTransaction transaction;
        PackageTransactionStatus status;
        CupState state;
        StateFileStatus state_status;

        package_transaction_init(&transaction);
        state_init(&state);
        err = package_transaction_load(&transaction, &status);
        if (err == CUP_OK && status != PACKAGE_TRANSACTION_LOADED) err = CUP_ERR_TRANSACTION;
        if (err == CUP_OK) {
            err = state_load(&state, &state_status, NULL, NULL);
            if (err != CUP_OK || state_status != STATE_FILE_LOADED) err = CUP_ERR_TRANSACTION;
        }
        if (err == CUP_OK) {
            err = state_validate(&state, NULL);
            if (err != CUP_OK) err = CUP_ERR_TRANSACTION;
        }
        if (err == CUP_OK) err = package_transaction_recover(&transaction, &state);
        state_free(&state);
    } else if (kind == RUNTIME_JOURNAL_GENERATION) {
        UpdateJournal journal;
        UpdateJournalStatus status;

        update_journal_init(&journal);
        err = update_journal_load(&journal, &status);
        if (err == CUP_OK && status != CUP_UPDATE_JOURNAL_LOADED) err = CUP_ERR_TRANSACTION;
        if (err == CUP_OK) err = update_journal_recover(&journal, NULL);
    } else if (kind == RUNTIME_JOURNAL_UNINSTALL) {
        UninstallJournal journal;
        UninstallJournalStatus status;
        char root[MAX_PATH_LEN];

        uninstall_journal_init(&journal);
        err = uninstall_journal_load(&journal, &status);
        if (err == CUP_OK && status != UNINSTALL_JOURNAL_LOADED) err = CUP_ERR_TRANSACTION;
        if (err == CUP_OK) err = layout_get_root(root, sizeof(root));
        if (err == CUP_OK) err = uninstall_helper_remove_stale(root, journal.token, lock);
        if (err == CUP_OK) err = uninstall_journal_recover(&journal);
    } else {
        err = CUP_ERR_TRANSACTION;
    }

    if (err != CUP_OK) {
        fprintf(stderr, "Error: interrupted operation cannot be recovered safely.\n");
        return err;
    }
    return CUP_OK;
}
