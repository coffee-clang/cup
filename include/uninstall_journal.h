#ifndef CUP_UNINSTALL_JOURNAL_H
#define CUP_UNINSTALL_JOURNAL_H

/* Strict transaction.txt schema for the detached uninstall handoff. */

#include "constants.h"
#include "error.h"
#include "system.h"

typedef enum {
    UNINSTALL_JOURNAL_MISSING,
    UNINSTALL_JOURNAL_LOADED
} UninstallJournalStatus;

typedef enum {
    UNINSTALL_PHASE_SCHEDULED,
    UNINSTALL_PHASE_DETACHING,
    UNINSTALL_PHASE_FAILED
} UninstallPhase;

typedef enum {
    UNINSTALL_STAGE_HANDOFF,
    UNINSTALL_STAGE_PARENT_WAIT,
    UNINSTALL_STAGE_DETACH,
    UNINSTALL_STAGE_CLEANUP
} UninstallStage;

typedef struct {
    char temporary_name[MAX_PATH_LEN];
    char token[MAX_TRANSACTION_TOKEN_LEN];
    UninstallPhase phase;
    UninstallStage stage;
    int error_code;
    SystemPathIdentity file_identity;
} UninstallJournal;

void uninstall_journal_init(UninstallJournal *journal);
const char *uninstall_phase_name(UninstallPhase phase);
const char *uninstall_stage_name(UninstallStage stage);
CupError uninstall_journal_begin(const char *temporary_path, const char *token);
CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status);
/* Recover a stale uninstall journal while the caller owns the canonical exclusive cup lock.
 * Recovery never touches a detached root and succeeds only while that sibling is absent. */
CupError uninstall_journal_recover(const UninstallJournal *journal);

#endif /* CUP_UNINSTALL_JOURNAL_H */
