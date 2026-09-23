#ifndef CUP_RELEASE_METADATA_H
#define CUP_RELEASE_METADATA_H

/* Canonical format-2 release manifest shared by release, installer and self-update. */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Three six-digit numeric parts plus separators/NUL; one 40-hex Git commit plus NUL. */
#define CUP_RELEASE_VERSION_MAX 21
#define CUP_RELEASE_COMMIT_MAX 41
#define CUP_RELEASE_MANIFEST_MAX_BYTES (64u * 1024u)

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
} ReleaseVersion;

typedef struct {
    char name[MAX_PATH_SEGMENT_LEN];
    char sha256[65];
} ReleaseAsset;

typedef struct {
    char version[CUP_RELEASE_VERSION_MAX];
    char commit[CUP_RELEASE_COMMIT_MAX];
    unsigned root_layout;
    unsigned catalog_format;
    ReleaseAsset *assets;
    size_t asset_count;
} ReleaseMetadata;

/* Validate one canonical x.y.z release version and optionally return its numeric parts. */
CupError release_version_parse(const char *text, ReleaseVersion *version);

void release_metadata_init(ReleaseMetadata *metadata);
void release_metadata_free(ReleaseMetadata *metadata);
CupError release_metadata_load(const char *path, ReleaseMetadata *metadata);
const ReleaseAsset *release_metadata_find_asset(const ReleaseMetadata *metadata, const char *name);

#endif /* CUP_RELEASE_METADATA_H */
