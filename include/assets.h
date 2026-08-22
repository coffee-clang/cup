#ifndef CUP_ASSETS_H
#define CUP_ASSETS_H

/*
 * Inspection and checksum verification for the canonical executable, catalog, install policy
 * and checksum files. The native update helper is reported separately as derived data.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Integrity state of one installed asset. */
typedef enum {
    CUP_ASSET_MISSING,
    CUP_ASSET_VALID,
    CUP_ASSET_INVALID
} AssetStatus;

/* Complete read-only inspection of canonical assets, derived helper and development data. */
typedef struct {
    AssetStatus binary;
    AssetStatus helper;
    AssetStatus catalog;
    AssetStatus install_policy;
    AssetStatus common_checksums;
    AssetStatus platform_checksums;
    int development_catalog_valid;
    int development_install_policy_valid;
} AssetsInspection;

/* Inspect every managed asset without repairing or replacing any file. */
CupError assets_inspect(AssetsInspection *inspection);

/* Predicates over a completed AssetsInspection. */
int assets_has_installed_assets(const AssetsInspection *inspection);
int assets_installed_is_valid(const AssetsInspection *inspection);
int assets_development_is_valid(const AssetsInspection *inspection);

/* Build platform-dependent names used by checksum files and installers. */
CupError assets_binary_asset_name(char *name, size_t size);
CupError assets_platform_checksums_name(char *name, size_t size);

#endif /* CUP_ASSETS_H */
