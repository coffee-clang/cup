#ifndef CUP_PACKAGE_TRANSACTION_H
#define CUP_PACKAGE_TRANSACTION_H

/* Persistent package-operation journal and deterministic recovery API. */

#include "error.h"
#include "package.h"
#include "state.h"
#include "system.h"

typedef enum {
    PACKAGE_OPERATION_NONE,
    PACKAGE_OPERATION_INSTALL,
    PACKAGE_OPERATION_REMOVE,
    PACKAGE_OPERATION_UPDATE
} PackageOperation;

typedef enum {
    PACKAGE_TRANSACTION_MISSING,
    PACKAGE_TRANSACTION_LOADED
} PackageTransactionStatus;

typedef struct {
    PackageOperation operation;
    PackageIdentity package;
    char temporary_name[MAX_PATH_LEN];
    SystemPathIdentity file_identity;
} PackageTransaction;

/* Journal lifecycle and owner-specific deterministic recovery. */
void package_transaction_init(PackageTransaction *transaction);
CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path,
                                   PackageTransaction *created);
CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status);
/*
 * Recover only a transaction produced by begin/load against state that already passed
 * state_validate(). Recovery retains transaction.txt unless the state-determined filesystem result
 * is complete and the exact loaded journal identity can be removed durably. Retryable partial
 * filesystem progress is reported as CUP_ERR_COMMIT; explicit restoration failure is preserved.
 */
CupError package_transaction_recover(const PackageTransaction *transaction, CupState *state);

/* Stable diagnostic name for one package operation. */
const char *package_operation_name(PackageOperation operation);

#endif /* CUP_PACKAGE_TRANSACTION_H */
