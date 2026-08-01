/*
 * Validates the canonical CUP assets, records uninstall in transaction.txt and starts the detached
 * platform helper that removes the root after the current process exits.
 */

#include "commands.h"

#include "cup_assets.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"
#include "uninstall_journal.h"

#include <stdio.h>
#include <string.h>

/* Interactive confirmation is kept outside the detached helper. */
static int confirm_uninstall(const char *root_path) {
    char answer[16];

    printf("This will remove cup and all cup-managed data from:\n  %s\n\n", root_path);
    printf("The PATH entry will not be removed.\nContinue? [y/N] ");
    fflush(stdout);

    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        return 0;
    }

    return answer[0] == 'y' || answer[0] == 'Y';
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
    temporary_path[(size_t)(name - temporary_path) + 14] = '.';
    name = path_last_segment(temporary_path);
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

/* Validate, journal and delegate post-exit removal. */
CupError command_uninstall(int assume_yes) {
    RuntimeJournalKind journal_kind;
    SystemLock lock = {0, 0};
    CupAssetsSource source;
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char script_path[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN];
    char token[MAX_IDENTIFIER_LEN];
    unsigned long parent_pid;
    int root_is_directory;
    int journal_created = 0;

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
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    /* Adopt a fully verified legacy root before the detached helper validates ownership. */
    err = layout_ensure_root();
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

    err = cup_assets_find_uninstall(script_path, sizeof(script_path), &source);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: no valid installed or development uninstall script was found.\n");
        goto done;
    }
    (void)source;

    if (!assume_yes && !confirm_uninstall(root_path)) {
        printf("Uninstall cancelled.\n");
        err = CUP_OK;
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

    parent_pid = system_get_process_id();
    err = system_start_uninstall(root_path, script_path, parent_pid);
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
        UninstallJournal journal;
        UninstallJournalStatus status;
        CupError load_error = uninstall_journal_load(&journal, &status);

        if (load_error == CUP_OK && status == UNINSTALL_JOURNAL_LOADED &&
            uninstall_journal_is_initial(&journal, temporary_path, token)) {
            CupError clear_error = runtime_journal_clear();

            if (err == CUP_OK && clear_error != CUP_OK) {
                err = clear_error;
            }
        }
    }
    system_lock_release(&lock);
    return err;
}
