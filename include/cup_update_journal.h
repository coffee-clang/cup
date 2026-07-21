#ifndef CUP_UPDATE_JOURNAL_H
#define CUP_UPDATE_JOURNAL_H

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
    CUP_UPDATE_RESULT_MISSING,
    CUP_UPDATE_RESULT_SUCCESS,
    CUP_UPDATE_RESULT_FAILED
} CupUpdateResultStatus;

typedef struct {
    char temporary_name[MAX_PATH_LEN];
    char token[MAX_PATH_LEN];
    char version[MAX_IDENTIFIER_LEN];
    CupUpdatePhase phase;
    int error_code;
} CupUpdateJournal;

typedef struct {
    CupUpdateResultStatus status;
    int error_code;
    char version[MAX_IDENTIFIER_LEN];
} CupUpdateResult;

void cup_update_journal_init(CupUpdateJournal *journal);
const char *cup_update_phase_name(CupUpdatePhase phase);
CupError cup_update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version);
CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code);
CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status);
CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
                                             char *buffer,
                                             size_t size);
CupError cup_update_journal_recover(const CupUpdateJournal *journal);
CupError cup_update_journal_clear(void);

void cup_update_result_init(CupUpdateResult *result);
CupError cup_update_result_write(CupUpdateResultStatus status, int error_code, const char *version);
CupError cup_update_result_load(CupUpdateResult *result);
CupError cup_update_result_report(void);

#endif /* CUP_UPDATE_JOURNAL_H */
