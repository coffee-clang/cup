#ifndef CUP_UPDATE_JOURNAL_H
#define CUP_UPDATE_JOURNAL_H

/*
 * Persists detached cup-update state in transaction.txt. The journal remains authoritative
 * until the new generation is finalized or repair acknowledges a failed rollback.
 */
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "system.h"

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
    char token[MAX_TRANSACTION_TOKEN_LEN];
    char version[MAX_IDENTIFIER_LEN];
    CupUpdatePhase phase;
    CupUpdateFailureRecovery recovery;
    int error_code;
    SystemPathIdentity file_identity;
} CupUpdateJournal;

/* In-progress, failed and acknowledged journal lifecycle. */
void cup_update_journal_init(CupUpdateJournal *journal);
const char *cup_update_phase_name(CupUpdatePhase phase);
const char *cup_update_failure_recovery_name(CupUpdateFailureRecovery recovery);
CupError cup_update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version,
                                  CupUpdateJournal *created);
CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code);
CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status);
CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
                                             char *buffer,
                                             size_t size);
CupError cup_update_write_generation_marker(const char *staging,
                                            const char *version,
                                            const char *staged_binary);
CupError cup_update_journal_recover(const CupUpdateJournal *journal,
                                    CupUpdateRecoveryMode mode,
                                    CupUpdateRecoveryResult *result);

#endif /* CUP_UPDATE_JOURNAL_H */
