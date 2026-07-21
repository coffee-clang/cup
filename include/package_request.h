#ifndef CUP_PACKAGE_REQUEST_H
#define CUP_PACKAGE_REQUEST_H

/*
 * Command-boundary package selector validation and catalog resolution without owning command
 * lifetime or installation policy.
 */

#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "package_catalog.h"
#include "package_selector.h"

typedef struct {
    PackageSelector selector;
    char resolved_release[MAX_IDENTIFIER_LEN];
    char input_selector[MAX_SELECTOR_LEN];
    char resolved_selector[MAX_SELECTOR_LEN];
} PackageRequest;

/* Parse CLI syntax, resolve symbolic stable, then render the chosen selector when needed. */
CupError package_request_parse(const char *component,
                               const char *selector_text,
                               PackageRequest *request);
CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host_platform,
                                 const char *target_platform,
                                 PackageRequest *request);
void package_request_print(FILE *stream, const PackageRequest *request);

#endif /* CUP_PACKAGE_REQUEST_H */
