#ifndef CUP_PACKAGE_MANIFEST_H
#define CUP_PACKAGE_MANIFEST_H

/*
 * Verifies the producer-owned manifest.txt format=2 inventory of one extracted package.
 * The manifest itself is deliberately outside the inventory; every other package object must be
 * declared exactly once with its logical type, mode and SHA-256 identity.
 */

#include <stdio.h>

#include "error.h"

CupError package_manifest_verify(const char *base_path,
                                 const char *host_platform,
                                 FILE *diagnostics);

#endif /* CUP_PACKAGE_MANIFEST_H */
