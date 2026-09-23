#ifndef CUP_CATALOG_REFRESH_H
#define CUP_CATALOG_REFRESH_H

/*
 * Refresh the installed catalog without holding the runtime lock across network I/O.
 * The caller must already have selected/pinned one root for the public command lifetime.
 */

#include "error.h"

typedef enum {
    CATALOG_REFRESH_REPORT_ERRORS,
    CATALOG_REFRESH_QUIET
} CatalogRefreshDiagnostics;

/* Refresh an existing runtime catalog. `updated` is set when newer bytes were committed. */
CupError catalog_refresh_existing(int *updated, CatalogRefreshDiagnostics diagnostics);

#endif /* CUP_CATALOG_REFRESH_H */
