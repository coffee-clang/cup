#ifndef CUP_RELEASE_METADATA_H
#define CUP_RELEASE_METADATA_H

/* Canonical immutable release.txt metadata shared by update and bootstrap. */

#include "error.h"

/* Three six-digit numeric parts plus separators/NUL; one 40-hex Git commit plus NUL. */
#define CUP_RELEASE_VERSION_MAX 21
#define CUP_RELEASE_COMMIT_MAX 41

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
} ReleaseVersion;

typedef struct {
    char version[CUP_RELEASE_VERSION_MAX];
    char commit[CUP_RELEASE_COMMIT_MAX];
} ReleaseMetadata;

/* Validate one canonical x.y.z release version and optionally return its numeric parts. */
CupError release_version_parse(const char *text, ReleaseVersion *version);
CupError release_metadata_load(const char *path, ReleaseMetadata *metadata);

#endif /* CUP_RELEASE_METADATA_H */
