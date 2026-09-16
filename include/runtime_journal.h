#ifndef CUP_RUNTIME_JOURNAL_H
#define CUP_RUNTIME_JOURNAL_H

/* Shared transaction.txt transport. This module owns durable I/O and identity-bound removal;
 * typed journal modules own schema, consistency and recovery. */

#include <stddef.h>
#include <stdio.h>

#include "error.h"
#include "system.h"

typedef enum {
    RUNTIME_JOURNAL_MISSING,
    RUNTIME_JOURNAL_PACKAGE,
    RUNTIME_JOURNAL_UPDATE,
    RUNTIME_JOURNAL_UNINSTALL
} RuntimeJournalKind;

typedef CupError (*RuntimeJournalWriter)(FILE *file, const void *value);
typedef CupError (*RuntimeJournalFieldVisitor)(const char *key,
                                               const char *value,
                                               void *userdata);

/* Shared transaction-token grammar used by detached update and uninstall journals. */
int runtime_journal_token_is_valid(const char *token);

/* Parse the common key=value envelope and delegate typed fields to the visitor. `ordered_keys`
 * optionally enforces the complete field order. */
CupError runtime_journal_parse_at(const char *root,
                                  const char *const *ordered_keys,
                                  size_t ordered_key_count,
                                  RuntimeJournalFieldVisitor visitor,
                                  void *userdata,
                                  SystemPathIdentity *identity,
                                  int *missing);
CupError runtime_journal_parse(const char *const *ordered_keys,
                               size_t ordered_key_count,
                               RuntimeJournalFieldVisitor visitor,
                               void *userdata,
                               SystemPathIdentity *identity,
                               int *missing);

/* Publish through a temporary file. NULL `expected_identity` is create-only; replacement is
 * identity-bound. CUP_ERR_COMMIT may still return a proven new identity. */
CupError runtime_journal_publish_at(const char *root,
                                    const char *temporary_directory,
                                    const char *temporary_prefix,
                                    const SystemPathIdentity *expected_identity,
                                    RuntimeJournalWriter writer,
                                    const void *value,
                                    SystemPathIdentity *published_identity);
CupError runtime_journal_publish(const char *temporary_directory,
                                 const char *temporary_prefix,
                                 const SystemPathIdentity *expected_identity,
                                 RuntimeJournalWriter writer,
                                 const void *value,
                                 SystemPathIdentity *published_identity);

/* Detect the owner of transaction.txt without interpreting owner-specific fields. */
CupError runtime_journal_detect(RuntimeJournalKind *kind);

/* Remove only the retained regular-file journal identity. CUP_ERR_COMMIT means deletion was
 * applied but parent-directory durability is uncertain. */
CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity);

/* Reject operational commands while any valid or invalid journal is present. */
CupError runtime_journal_require_none(void);

#endif /* CUP_RUNTIME_JOURNAL_H */
