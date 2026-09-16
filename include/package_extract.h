#ifndef CUP_PACKAGE_EXTRACT_H
#define CUP_PACKAGE_EXTRACT_H

/* Extract one verified archive into fresh staging. Paths are validated against all supported
 * filesystem models; semantic metadata and manifest/link integrity are checked afterwards. */

#include "error.h"
#include "package_artifact.h"

/* Extract only the already authenticated bytes owned by one verified artifact. */
CupError package_extract_verified(VerifiedArtifact *artifact, const char *staging_path);

#endif /* CUP_PACKAGE_EXTRACT_H */
