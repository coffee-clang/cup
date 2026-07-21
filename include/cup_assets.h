#ifndef CUP_CUP_ASSETS_H
#define CUP_CUP_ASSETS_H

/*
 * Module contract: CUP assets inspection and checksum-verification contract
 * for the canonical cup executable, catalog, checksum files, and uninstall
 * helper.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Integrity state of one installed CUP asset. */
typedef enum {
    CUP_ASSET_MISSING,
    CUP_ASSET_VALID,
    CUP_ASSET_INVALID
} CupAssetStatus;

/* Source selected for an operation that can use installed or checkout data. */
typedef enum {
    CUP_ASSETS_SOURCE_NONE,
    CUP_ASSETS_SOURCE_INSTALLED,
    CUP_ASSETS_SOURCE_DEVELOPMENT
} CupAssetsSource;

/* Complete read-only inspection of installed and development CUP asset data. */
typedef struct {
    CupAssetStatus binary;
    CupAssetStatus helper;
    CupAssetStatus catalog;
    CupAssetStatus install_policy;
    CupAssetStatus uninstall;
    CupAssetStatus common_checksums;
    CupAssetStatus platform_checksums;
    int development_catalog_valid;
    int development_install_policy_valid;
    int development_uninstall_valid;
} CupAssetsInspection;

/* Inspect every CUP asset without repairing or replacing any file. */
CupError cup_assets_inspect(CupAssetsInspection *inspection);

/* Predicates over a completed CupAssetsInspection. */
int cup_assets_has_installed_assets(const CupAssetsInspection *inspection);
int cup_assets_installed_is_valid(const CupAssetsInspection *inspection);
int cup_assets_development_is_valid(const CupAssetsInspection *inspection);

/* Select a validated uninstall helper, preferring the installed asset. */
CupError cup_assets_find_uninstall(char *path, size_t size, CupAssetsSource *source);

/* Report whether a detached uninstall has marked the canonical root pending. */
CupError cup_assets_uninstall_is_pending(int *pending);

/* Build platform-dependent names used by checksum files and installers. */
CupError cup_assets_binary_asset_name(char *name, size_t size);
CupError cup_assets_platform_checksums_name(char *name, size_t size);

/* Verify one named asset against one already selected checksum file. */
CupError cup_assets_verify_asset(const char *checksum_path,
                                 const char *asset_name,
                                 const char *asset_path,
                                 int *matches);

#endif /* CUP_CUP_ASSETS_H */
