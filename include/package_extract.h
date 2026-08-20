#ifndef CUP_PACKAGE_EXTRACT_H
#define CUP_PACKAGE_EXTRACT_H

/*
 * Extracts one supported package archive into a fresh caller-owned staging directory. Entry paths
 * are checked against every supported filesystem model so they cannot escape or alias each other.
 */

#include "error.h"
#include "package_artifact.h"

/* Extract only the already authenticated bytes owned by one verified artifact. */
CupError package_extract_verified(VerifiedArtifact *artifact, const char *staging_path);

#endif /* CUP_PACKAGE_EXTRACT_H */
