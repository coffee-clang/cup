#ifndef CUP_UNINSTALL_JOURNAL_H
#define CUP_UNINSTALL_JOURNAL_H

/* Strict transaction.txt schema for the detached uninstall handoff. */

#include <stddef.h>

#include "constants.h"
#include "error.h"

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
    char token[MAX_IDENTIFIER_LEN];
    UninstallPhase phase;
    UninstallStage stage;
    int error_code;
} UninstallJournal;

void uninstall_journal_init(UninstallJournal *journal);
const char *uninstall_phase_name(UninstallPhase phase);
const char *uninstall_stage_name(UninstallStage stage);
CupError uninstall_journal_begin(const char *temporary_path, const char *token);
CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status);
CupError uninstall_journal_get_detached_path(const UninstallJournal *journal,
                                             char *buffer,
                                             size_t size);
CupError uninstall_journal_acknowledge_failure(const UninstallJournal *journal);

#endif /* CUP_UNINSTALL_JOURNAL_H */
