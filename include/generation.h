#ifndef CUP_GENERATION_H
#define CUP_GENERATION_H

/* One canonical installed cup generation: manifest, legal assets and executable. */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "release_metadata.h"

typedef enum {
    CUP_GENERATION_ASSET_RELEASE = 0,
    CUP_GENERATION_ASSET_LICENSE,
    CUP_GENERATION_ASSET_NOTICES,
    CUP_GENERATION_ASSET_BINARY,
    CUP_GENERATION_ASSET_COUNT
} GenerationAssetId;


typedef enum {
    CUP_GENERATION_ASSET_MISSING = 0,
    CUP_GENERATION_ASSET_VALID,
    CUP_GENERATION_ASSET_INVALID
} GenerationAssetStatus;

typedef struct {
    GenerationAssetStatus release;
    GenerationAssetStatus license;
    GenerationAssetStatus notices;
    GenerationAssetStatus binary;
} GenerationInspection;

typedef struct {
    GenerationAssetId id;
    char release_name[MAX_PATH_SEGMENT_LEN];
    char destination[MAX_PATH_LEN];
    int executable;
    int read_only;
} GenerationAssetSpec;

CupError generation_binary_release_name(char *name, size_t size);
CupError generation_asset_release_name(GenerationAssetId id, char *name, size_t size);
CupError generation_asset_spec(GenerationAssetId id, GenerationAssetSpec *spec);
CupError generation_asset_specs(GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT]);

CupError generation_inspect(GenerationInspection *inspection);
int generation_has_installed_assets(const GenerationInspection *inspection);
int generation_installed_is_valid(const GenerationInspection *inspection);

/* release.txt authenticates every generation asset except itself. */
const ReleaseAsset *generation_manifest_asset(const ReleaseMetadata *metadata,
                                              GenerationAssetId id);

/* Validate compatibility and the exact required installed-generation entries in one manifest. */
CupError generation_validate_manifest(const ReleaseMetadata *metadata);

#endif /* CUP_GENERATION_H */
