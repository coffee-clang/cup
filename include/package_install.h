#ifndef CUP_PACKAGE_INSTALL_H
#define CUP_PACKAGE_INSTALL_H

/*
 * Reusable single-package installation operation used by public install handlers and
 * stable-update plans.
 */

#include "error.h"
#include "package_artifact.h"

/* Execute one catalog-pinned artifact plan without re-resolving catalog coordinates. */
CupError package_install_artifact(const PackageArtifactSpec *spec);

/* Update one installed scope using its catalog-pinned stable artifact. */
CupError package_install_update_artifact(const PackageArtifactSpec *spec,
                                         const PackageIdentity *expected_default,
                                         int *installed,
                                         int *default_moved);

#endif /* CUP_PACKAGE_INSTALL_H */
