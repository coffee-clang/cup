#ifndef CUP_TRANSACTION_H
#define CUP_TRANSACTION_H

#include <stddef.h>

#include "error.h"
#include "package.h"
#include "state.h"

/* Mutating operations that can be recovered after interruption. */
typedef enum {
    TRANSACTION_NONE,
    TRANSACTION_INSTALL,
    TRANSACTION_REMOVE,
    TRANSACTION_SELF_UPDATE
} TransactionOperation;

/* Presence state of the persistent transaction journal. */
typedef enum {
    TRANSACTION_FILE_MISSING,
    TRANSACTION_FILE_LOADED
} TransactionFileStatus;

/* Persistent data required to recover one interrupted operation. */
typedef struct {
    TransactionOperation operation;
    PackageIdentity package;
    char temporary_name[MAX_PATH_LEN];
} Transaction;

/* Reset a transaction structure. */
void transaction_init(Transaction *transaction);

/* Create and persist a new transaction journal. */
CupError transaction_begin(TransactionOperation operation,
    const PackageIdentity *package, const char *temporary_path);

/* Persist a deferred self-update before the current process releases its lock. */
CupError transaction_begin_self_update(const char *temporary_path);

/* Load and fully validate the current transaction journal. */
CupError transaction_load(Transaction *transaction,
    TransactionFileStatus *status);

/* Rebuild the temporary path recorded by the journal. */
CupError transaction_get_tmp_path(const Transaction *transaction,
    char *buffer, size_t size);

/* Reconcile an interrupted operation using state.txt as the commit point. */
CupError transaction_recover(const Transaction *transaction, CupState *state);

/* Remove the journal after commit, rollback or recovery. */
CupError transaction_clear(void);

/* Return the stable textual name stored in the journal. */
const char *transaction_operation_name(TransactionOperation operation);

#endif /* CUP_TRANSACTION_H */
