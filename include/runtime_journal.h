#ifndef CUP_RUNTIME_JOURNAL_H
#define CUP_RUNTIME_JOURNAL_H

/*
 * transaction.txt is shared physically by package operations and deferred CUP updates.
 * This boundary identifies the owner without parsing owner-specific fields and provides the
 * common command blocker used before mutable state is opened.
 */

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
