#ifndef CUP_BOOTSTRAP_H
#define CUP_BOOTSTRAP_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef enum {
    BOOTSTRAP_ASSET_MISSING,
    BOOTSTRAP_ASSET_VALID,
    BOOTSTRAP_ASSET_INVALID
} BootstrapAssetStatus;

typedef enum {
    BOOTSTRAP_SOURCE_NONE,
    BOOTSTRAP_SOURCE_INSTALLED,
    BOOTSTRAP_SOURCE_DEVELOPMENT
} BootstrapSource;

typedef struct {
    BootstrapAssetStatus binary;
    BootstrapAssetStatus manifest;
    BootstrapAssetStatus uninstall;
    BootstrapAssetStatus common_checksums;
    BootstrapAssetStatus platform_checksums;
    int development_manifest_valid;
    int development_uninstall_valid;
} BootstrapInspection;

CupError bootstrap_inspect(BootstrapInspection *inspection);
int bootstrap_has_installed_assets(const BootstrapInspection *inspection);
int bootstrap_installed_is_valid(const BootstrapInspection *inspection);
int bootstrap_development_is_valid(const BootstrapInspection *inspection);
CupError bootstrap_find_uninstall(char *path, size_t size,
    BootstrapSource *source);
CupError bootstrap_uninstall_is_pending(int *pending);
CupError bootstrap_binary_asset_name(char *name, size_t size);
CupError bootstrap_platform_checksums_name(char *name, size_t size);
CupError bootstrap_verify_asset(const char *checksum_path,
    const char *asset_name, const char *asset_path, int *matches);

#endif /* CUP_BOOTSTRAP_H */
