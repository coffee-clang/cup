#ifndef CUP_INSTALLED_PACKAGE_H
#define CUP_INSTALLED_PACKAGE_H

/*
 * Reconciles one concrete package identity between persistent state and the canonical components
 * tree, without owning command lifetime or output.
 */

#include "error.h"
#include "package.h"
#include "state.h"

/* Require a package to be present both in state and on disk. */
CupError installed_package_require_present(const CupState *state, const PackageIdentity *package);

/* Require an installed package to pass full on-disk validation. */
CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package);

/* Require a package to be absent both from state and disk. */
CupError installed_package_require_absent(const CupState *state, const PackageIdentity *package);

#endif /* CUP_INSTALLED_PACKAGE_H */
