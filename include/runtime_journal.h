#ifndef CUP_RUNTIME_JOURNAL_H
#define CUP_RUNTIME_JOURNAL_H

#include "error.h"

typedef enum {
    RUNTIME_JOURNAL_MISSING,
    RUNTIME_JOURNAL_PACKAGE,
    RUNTIME_JOURNAL_CUP_UPDATE
} RuntimeJournalKind;

/* Detect the owner of transaction.txt without interpreting owner-specific fields. */
CupError runtime_journal_detect(RuntimeJournalKind *kind);

/* Reject operational commands while any valid or invalid journal is present. */
CupError runtime_journal_require_none(void);

#endif /* CUP_RUNTIME_JOURNAL_H */
