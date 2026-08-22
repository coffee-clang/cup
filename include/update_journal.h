#ifndef CUP_UPDATE_JOURNAL_H
#define CUP_UPDATE_JOURNAL_H

/*
 * Persists detached self-update state in transaction.txt. The journal remains authoritative
 * until the new generation is finalized or repair acknowledges a failed rollback.
 */
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "system.h"

typedef enum {
    CUP_UPDATE_JOURNAL_MISSING,
    CUP_UPDATE_JOURNAL_LOADED
} UpdateJournalStatus;

typedef enum {
    CUP_UPDATE_PHASE_SCHEDULED,
    CUP_UPDATE_PHASE_COMMITTING,
    CUP_UPDATE_PHASE_FAILED
} UpdatePhase;

typedef enum {
    CUP_UPDATE_FAILURE_NONE,
    CUP_UPDATE_FAILURE_PENDING,
    CUP_UPDATE_FAILURE_ROLLED_BACK
} UpdateFailureRecovery;

typedef enum {
    CUP_UPDATE_RECOVER_REPLACE_BINARY,
    CUP_UPDATE_RECOVER_PRESERVE_BINARY
} UpdateRecoveryMode;

typedef enum {
    CUP_UPDATE_RECOVERY_NONE,
    CUP_UPDATE_RECOVERY_FINALIZED,
    CUP_UPDATE_RECOVERY_ROLLED_BACK,
    CUP_UPDATE_RECOVERY_ACKNOWLEDGED
} UpdateRecoveryResult;

typedef struct {
    char temporary_name[MAX_PATH_LEN];
    char token[MAX_TRANSACTION_TOKEN_LEN];
    char version[MAX_IDENTIFIER_LEN];
    UpdatePhase phase;
    UpdateFailureRecovery recovery;
    int error_code;
    SystemPathIdentity file_identity;
} UpdateJournal;

/* In-progress, failed and acknowledged journal lifecycle. */
void update_journal_init(UpdateJournal *journal);
const char *update_phase_name(UpdatePhase phase);
const char *update_failure_recovery_name(UpdateFailureRecovery recovery);
CupError update_journal_begin(const char *temporary_path,
                              const char *token,
                              const char *version,
                              UpdateJournal *created);
CupError update_journal_set_phase(UpdateJournal *journal,
                                  UpdatePhase phase,
                                  int error_code);
CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status);
CupError update_journal_get_staging_path(const UpdateJournal *journal,
                                         char *buffer,
                                         size_t size);
CupError update_write_generation_marker(const char *staging,
                                        const char *version,
                                        const char *staged_binary);
CupError update_journal_recover(const UpdateJournal *journal,
                                UpdateRecoveryMode mode,
                                UpdateRecoveryResult *result);

#endif /* CUP_UPDATE_JOURNAL_H */
