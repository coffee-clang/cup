#ifndef CUP_PACKAGE_MANIFEST_H
#define CUP_PACKAGE_MANIFEST_H

/* Verify manifest.txt format=2: every other package object appears exactly once, and POSIX
 * links resolve inside the package to regular files. */

#include <stdio.h>

#include "error.h"

CupError package_manifest_verify(const char *base_path,
                                 const char *host_platform,
                                 FILE *diagnostics);

#endif /* CUP_PACKAGE_MANIFEST_H */
