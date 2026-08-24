/*
 * Validates the managed root, records uninstall in transaction.txt and hands exclusive authority
 * to a temporary native copy. The helper owns root detach/cleanup after this process exits; on
 * Windows its temporary executable deletion is armed separately before the root can be mutated.
 */

#include "commands.h"

#include "interrupt.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"
#include "uninstall_helper.h"
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

static CupError clear_unstarted_journal(const char *root,
                                        const char *temporary_path,
                                        const char *token,
                                        const SystemLock *lock) {
    UninstallJournal journal;
    UninstallJournalStatus status;
    CupError err;

    if (text_is_empty(root) || lock == NULL || !lock->active ||
        lock->mode != SYSTEM_LOCK_EXCLUSIVE) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = uninstall_journal_load(&journal, &status);
    if (err != CUP_OK || status != UNINSTALL_JOURNAL_LOADED ||
        !uninstall_journal_is_initial(&journal, temporary_path, token)) {
        return CUP_OK;
    }
    /* The journal owns any pre-detach helper residue. Never erase that ownership evidence until
     * the exact token-bound helper has either disappeared or been removed under this lock. */
    err = uninstall_helper_remove_stale(root, token, lock);
    if (err != CUP_OK) {
        return err;
    }
    return runtime_journal_clear_if_identity(&journal.file_identity);
}

/* Validate, journal and begin the platform-specific uninstall handoff. */
CupError command_uninstall(int assume_yes) {
    RuntimeJournalKind journal_kind;
    SystemLock lock = {0};
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
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
    /* Success consumes the canonical lock only after the child already owns equivalent handoff
     * authority. From that point the helper owns journal recovery and root destruction; Windows
     * also carries the parent's exact DELETE_ON_CLOSE handle for its own later cleanup. */
    err = uninstall_helper_start(root_path, temporary_path, token, &lock);
    if (err == CUP_OK) {
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
        if (clear_unstarted_journal(root_path, temporary_path, token, &lock) != CUP_OK) {
            fprintf(stderr,
                    "Error: uninstall handoff cleanup was incomplete. Run 'cup repair'.\n");
            err = CUP_ERR_TRANSACTION;
        }
    }
    system_lock_release(&lock);
    return err;
}
