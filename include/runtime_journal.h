#ifndef CUP_RUNTIME_JOURNAL_H
#define CUP_RUNTIME_JOURNAL_H

/*
 * Package operations, detached cup updates and uninstall share one physical transaction.txt.
 * This module owns reading, durable publication and identity-bound removal of that file.
 * Typed journal modules own their schemas, consistency rules and recovery behavior.
 */

#include <stddef.h>
#include <stdio.h>

#include "error.h"
#include "system.h"

typedef enum {
    RUNTIME_JOURNAL_MISSING,
    RUNTIME_JOURNAL_PACKAGE,
    RUNTIME_JOURNAL_CUP_UPDATE,
    RUNTIME_JOURNAL_UNINSTALL
} RuntimeJournalKind;

typedef CupError (*RuntimeJournalWriter)(FILE *file, const void *value);
typedef CupError (*RuntimeJournalFieldVisitor)(const char *key,
                                               const char *value,
                                               void *userdata);

/*
 * Parse the common key=value envelope and delegate owner-specific fields to a typed visitor.
 * When ordered_keys is non-NULL, the parser also enforces the complete declared field order.
 * Journal values use a closed token grammar and therefore cannot contain spaces.
 */
CupError runtime_journal_parse(const char *const *ordered_keys,
                               size_t ordered_key_count,
                               RuntimeJournalFieldVisitor visitor,
                               void *userdata,
                               SystemPathIdentity *identity,
                               int *missing);

/*
 * Serialize one typed journal into a temporary file in the caller-provided managed directory and
 * publish it as transaction.txt.
 * expected_identity is NULL for first publication and mandatory for identity-bound replacement.
 * CUP_ERR_COMMIT means the new file may be visible and published_identity is populated when its
 * identity can still be proven.
 */
CupError runtime_journal_publish(const char *temporary_directory,
                                 const char *temporary_prefix,
                                 const SystemPathIdentity *expected_identity,
                                 RuntimeJournalWriter writer,
                                 const void *value,
                                 SystemPathIdentity *published_identity);

/* Detect the owner of transaction.txt without interpreting owner-specific fields. */
CupError runtime_journal_detect(RuntimeJournalKind *kind);

/*
 * Remove only the regular-file journal identity retained by its typed parser or creator.
 * CUP_ERR_COMMIT means deletion was applied but parent-directory durability could not be proved.
 */
CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity);

/* Reject operational commands while any valid or invalid journal is present. */
CupError runtime_journal_require_none(void);

#endif /* CUP_RUNTIME_JOURNAL_H */
