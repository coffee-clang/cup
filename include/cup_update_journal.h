#ifndef CUP_UPDATE_JOURNAL_H
#define CUP_UPDATE_JOURNAL_H

/*
 * Persists the detached CUP-update protocol in transaction.txt. The journal remains the
 * authoritative record until a committed generation is finalized or a failed rollback is
 * explicitly acknowledged by repair.
 */
#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef enum {
    CUP_UPDATE_JOURNAL_MISSING,
    CUP_UPDATE_JOURNAL_LOADED
} CupUpdateJournalStatus;

typedef enum {
    CUP_UPDATE_PHASE_SCHEDULED,
    CUP_UPDATE_PHASE_COMMITTING,
    CUP_UPDATE_PHASE_FAILED
} CupUpdatePhase;

typedef enum {
    CUP_UPDATE_FAILURE_NONE,
    CUP_UPDATE_FAILURE_PENDING,
    CUP_UPDATE_FAILURE_ROLLED_BACK
} CupUpdateFailureRecovery;

typedef enum {
    CUP_UPDATE_RECOVER_REPLACE_BINARY,
    CUP_UPDATE_RECOVER_PRESERVE_BINARY
} CupUpdateRecoveryMode;

typedef enum {
    CUP_UPDATE_RECOVERY_NONE,
    CUP_UPDATE_RECOVERY_FINALIZED,
    CUP_UPDATE_RECOVERY_ROLLED_BACK,
    CUP_UPDATE_RECOVERY_ACKNOWLEDGED
} CupUpdateRecoveryResult;

typedef struct {
    char temporary_name[MAX_PATH_LEN];
    char token[MAX_PATH_LEN];
    char version[MAX_IDENTIFIER_LEN];
    CupUpdatePhase phase;
    CupUpdateFailureRecovery recovery;
    int error_code;
} CupUpdateJournal;

/* In-progress, failed and acknowledged journal lifecycle. */
void cup_update_journal_init(CupUpdateJournal *journal);
const char *cup_update_phase_name(CupUpdatePhase phase);
const char *cup_update_failure_recovery_name(CupUpdateFailureRecovery recovery);
CupError cup_update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version);
CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code);
CupError cup_update_journal_set_recovery(CupUpdateJournal *journal,
                                         CupUpdateFailureRecovery recovery);
CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status);
CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
                                             char *buffer,
                                             size_t size);
CupError cup_update_journal_recover(const CupUpdateJournal *journal,
                                    CupUpdateRecoveryMode mode,
                                    CupUpdateRecoveryResult *result);

#endif /* CUP_UPDATE_JOURNAL_H */
