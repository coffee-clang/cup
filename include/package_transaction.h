#ifndef CUP_PACKAGE_TRANSACTION_H
#define CUP_PACKAGE_TRANSACTION_H

/* Persistent package-operation journal and deterministic recovery API. */

#include <stddef.h>

#include "error.h"
#include "package.h"
#include "state.h"

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
} PackageTransaction;

/* Journal lifecycle and owner-specific deterministic recovery. */
void package_transaction_init(PackageTransaction *transaction);
CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path);
CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status);
CupError package_transaction_get_staging_path(const PackageTransaction *transaction,
                                              char *buffer,
                                              size_t size);
CupError package_transaction_recover(const PackageTransaction *transaction, CupState *state);
CupError package_transaction_clear(void);

/* Stable diagnostic name for one package operation. */
const char *package_operation_name(PackageOperation operation);

#endif /* CUP_PACKAGE_TRANSACTION_H */
