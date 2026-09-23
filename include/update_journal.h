#ifndef CUP_UPDATE_JOURNAL_H
#define CUP_UPDATE_JOURNAL_H

/* Minimal format-2 cup-generation transaction journal and byte-evidence recovery. */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "system.h"

typedef enum {
    CUP_UPDATE_JOURNAL_MISSING,
    CUP_UPDATE_JOURNAL_LOADED
} UpdateJournalStatus;

typedef struct {
    char temporary_name[MAX_METADATA_VALUE_LEN];
    char target_release_sha256[65];
    SystemPathIdentity file_identity;
} UpdateJournal;

void update_journal_init(UpdateJournal *journal);
CupError update_journal_begin(const char *temporary_path,
                              const char *target_release_sha256,
                              UpdateJournal *created);
CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status);

/* `new/` must already contain the complete target generation. This verifies it and snapshots every
 * currently existing installed-generation asset into `old/` before journal publication. */
CupError update_generation_prepare(const char *staging,
                                   char target_release_sha256[65]);

/* Helper-side commit and ordinary recovery. Both retain journal/workspace evidence on ambiguity. */
CupError update_generation_commit(const UpdateJournal *journal);
CupError update_journal_recover(const UpdateJournal *journal, int *finalized);

#endif /* CUP_UPDATE_JOURNAL_H */
