#ifndef CUP_PACKAGE_EXTRACT_H
#define CUP_PACKAGE_EXTRACT_H

/* Extract one verified archive into fresh staging. Paths are validated against all supported
 * filesystem models; semantic metadata and manifest/link integrity are checked afterwards. */

#include <stddef.h>

#include "error.h"
#include "package_artifact.h"

/* Optional observer reports completed archive entries without owning presentation policy. */
typedef void (*PackageExtractProgress)(size_t extracted_count, void *userdata);

/* Extract only the already authenticated bytes owned by one verified artifact. */
CupError package_extract_verified(VerifiedArtifact *artifact, const char *staging_path);
CupError package_extract_verified_with_progress(VerifiedArtifact *artifact,
                                                const char *staging_path,
                                                PackageExtractProgress progress,
                                                void *progress_data);

#endif /* CUP_PACKAGE_EXTRACT_H */
