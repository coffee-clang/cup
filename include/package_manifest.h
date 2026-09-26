#ifndef CUP_PACKAGE_MANIFEST_H
#define CUP_PACKAGE_MANIFEST_H

/* Verify manifest.txt format=2: every other package object appears exactly once, and POSIX
 * links resolve inside the package to regular files. */

#include <stddef.h>
#include <stdio.h>

#include "error.h"

/* Optional observer reports verified manifest entries without owning presentation policy. */
typedef void (*PackageManifestProgress)(size_t verified_count, void *userdata);

CupError package_manifest_verify(const char *base_path,
                                 const char *host_platform,
                                 FILE *diagnostics);
CupError package_manifest_verify_with_progress(const char *base_path,
                                               const char *host_platform,
                                               FILE *diagnostics,
                                               PackageManifestProgress progress,
                                               void *progress_data);

#endif /* CUP_PACKAGE_MANIFEST_H */
