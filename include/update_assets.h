#ifndef CUP_UPDATE_ASSETS_H
#define CUP_UPDATE_ASSETS_H

/* Canonical five-asset generation used by bootstrap, self-update, update commit and recovery. */

#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef enum {
    CUP_UPDATE_ASSET_BINARY = 0,
    CUP_UPDATE_ASSET_PLATFORM_CHECKSUMS,
    CUP_UPDATE_ASSET_PACKAGES,
    CUP_UPDATE_ASSET_INSTALL_POLICY,
    CUP_UPDATE_ASSET_COMMON_CHECKSUMS,
    CUP_UPDATE_ASSET_COUNT
} UpdateAssetId;

typedef struct {
    const char *new_name;
    const char *old_name;
    const char *absent_name;
    const char *generation_key;
    int executable;
    int read_only;
} UpdateAssetSpec;

/* Return the static protocol record for one canonical generation asset. */
CupError update_asset_spec(UpdateAssetId id, UpdateAssetSpec *spec);

/* Return all five static records in canonical generation-marker/commit order. */
CupError update_asset_specs(UpdateAssetSpec specs[CUP_UPDATE_ASSET_COUNT]);

/* Resolve the installed destination only for consumers that actually need it. */
CupError update_asset_destination(UpdateAssetId id, char *path, size_t size);

/* Resolve the corresponding public release asset name without imposing identifier-size limits. */
CupError update_asset_release_name(UpdateAssetId id, char *name, size_t size);

#endif /* CUP_UPDATE_ASSETS_H */
