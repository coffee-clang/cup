/*
 * Validates the canonical cup assets, records uninstall in transaction.txt and starts the native
 * handoff. POSIX detaches the root in this process; Windows delegates detach to the helper. Cleanup
 * of the detached tree continues after the current process exits.
 */

#include "commands.h"

#include "cup_assets.h"
#include "interrupt.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"
#include "uninstall_journal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Interactive confirmation distinguishes a normal EOF/cancellation from an interrupt or I/O
 * failure. The caller must not report an interrupted read as a successful cancellation. */
static CupError confirm_uninstall(const char *root_path, int *confirmed) {
    char answer[16];

    if (confirmed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *confirmed = 0;
    printf("This will remove cup and all cup-managed data from:\n  %s\n\n", root_path);
    printf("The PATH entry will not be removed.\nContinue? [y/N] ");
    fflush(stdout);

    errno = 0;
    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        if (interrupt_requested() || errno == EINTR) {
            clearerr(stdin);
            return CUP_ERR_INTERRUPT;
        }
        if (ferror(stdin)) {
            clearerr(stdin);
            return CUP_ERR_FILESYSTEM;
        }
        return CUP_OK;
    }

    *confirmed = answer[0] == 'y' || answer[0] == 'Y';
    return CUP_OK;
}

static CupError prepare_uninstall_identity(const char *root,
                                           char *temporary_path,
                                           size_t temporary_size,
                                           char *token,
                                           size_t token_size) {
    char parent[MAX_PATH_LEN];
    const char *name;
    CupError err;

    err = path_parent(parent, sizeof(parent), root);
    if (err == CUP_OK) {
        err = system_make_unique_temp_path(
            parent, ".cup-uninstall", temporary_path, temporary_size);
    }
    if (err != CUP_OK) {
        return err;
    }
    name = path_last_segment(temporary_path);
    if (strncmp(name, ".cup-uninstall-", 15) != 0 || name[15] == '\0') {
        return CUP_ERR_TEMPORARY;
    }
    return text_copy(token, token_size, name + 15);
}

static int uninstall_journal_is_initial(const UninstallJournal *journal,
                                        const char *temporary_path,
                                        const char *token) {
    return journal != NULL && temporary_path != NULL && token != NULL &&
           journal->phase == UNINSTALL_PHASE_SCHEDULED &&
           journal->stage == UNINSTALL_STAGE_HANDOFF && journal->error_code == 0 &&
           strcmp(journal->temporary_name, path_last_segment(temporary_path)) == 0 &&
           strcmp(journal->token, token) == 0;
}

static CupError clear_unstarted_journal(const char *temporary_path, const char *token) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    CupError err;

    err = uninstall_journal_load(&journal, &status);
    if (err != CUP_OK || status != UNINSTALL_JOURNAL_LOADED ||
        !uninstall_journal_is_initial(&journal, temporary_path, token)) {
        return CUP_OK;
    }
    return runtime_journal_clear_if_identity(&journal.file_identity);
}

/* Clear only while this process still owns, or has safely reacquired, the canonical lock. */
static CupError clear_unstarted_journal_with_lock(SystemLock *lock,
                                                  const char *lock_path,
                                                  const char *temporary_path,
                                                  const char *token) {
    SystemLock cleanup_lock = {0, 0};
    CupError err;
    int acquired = 0;

    if (lock == NULL || text_is_empty(lock_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!lock->active) {
        err = system_lock_acquire_existing(&cleanup_lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err != CUP_OK) {
            return err;
        }
        acquired = 1;
        err = layout_root_snapshot_validate();
        if (err != CUP_OK) {
            system_lock_release(&cleanup_lock);
            return err;
        }
    }

    err = clear_unstarted_journal(temporary_path, token);
    if (acquired) {
        system_lock_release(&cleanup_lock);
    }
    return err;
}

/* Validate, journal and begin the platform-specific uninstall handoff. */
CupError command_uninstall(int assume_yes) {
    RuntimeJournalKind journal_kind;
    SystemLock lock = {0, 0};
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char script_path[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN];
    char token[MAX_TRANSACTION_TOKEN_LEN];
    int root_is_directory;
    int journal_created = 0;
    int confirmed = 0;

    err = layout_get_root(root_path, sizeof(root_path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_directory(root_path, &root_is_directory);
    if (err != CUP_OK) {
        return err;
    }
    if (!root_is_directory) {
        fprintf(stderr, "Error: cup has no managed data at '%s'.\n", root_path);
        return CUP_ERR_FILESYSTEM;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err != CUP_OK) {
        return err;
    }
    err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) {
        err = layout_root_snapshot_validate();
        if (err != CUP_OK) {
            system_lock_release(&lock);
        }
    }
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    /* Ensure the current root is initialized before the detached helper validates ownership. */
    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = layout_ensure_root();
    }
    if (err != CUP_OK) {
        goto done;
    }

    err = runtime_journal_detect(&journal_kind);
    if (err != CUP_OK || journal_kind != RUNTIME_JOURNAL_MISSING) {
        fprintf(stderr,
                "Error: an interrupted operation must be repaired before uninstalling cup.\n");
        err = CUP_ERR_TRANSACTION;
        goto done;
    }

    err = cup_assets_find_uninstall(script_path, sizeof(script_path));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: no valid installed or development uninstall script was found.\n");
        goto done;
    }

    if (!assume_yes) {
        err = confirm_uninstall(root_path, &confirmed);
        if (err != CUP_OK) {
            goto done;
        }
        if (!confirmed) {
            printf("Uninstall cancelled.\n");
            err = CUP_OK;
            goto done;
        }
    }

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        goto done;
    }
    err = prepare_uninstall_identity(root_path,
                                     temporary_path,
                                     sizeof(temporary_path),
                                     token,
                                     sizeof(token));
    if (err == CUP_OK) {
        err = uninstall_journal_begin(temporary_path, token);
    }
    if (err != CUP_OK) {
        goto done;
    }
    journal_created = 1;

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        goto done;
    }
    /* transaction.txt blocks new operations during handoff. system_start_uninstall() keeps
     * cup.lock authority through the canonical-root detach, either in this process or in the
     * Windows helper, so repair can distinguish an active handoff from a stale journal. */
    system_lock_release(&lock);
    err = system_start_uninstall(root_path, script_path, temporary_path, lock_path);
    if (err == CUP_OK || err == CUP_ERR_COMMIT) {
        printf("Uninstall started. The PATH entry was not removed.\n");
        journal_created = 0;
    }

done:
    /*
     * A failure before the helper validates the handoff leaves the exact initial journal behind
     * and it is safe to clear. Once the helper advances or records a failure, preserve that
     * evidence for doctor/repair instead of erasing it from the parent.
     */
    if (journal_created) {
        if (clear_unstarted_journal_with_lock(
                &lock, lock_path, temporary_path, token) != CUP_OK) {
            fprintf(stderr,
                    "Error: uninstall handoff cleanup was incomplete. Run 'cup repair'.\n");
            err = CUP_ERR_TRANSACTION;
        }
    }
    system_lock_release(&lock);
    return err;
}
