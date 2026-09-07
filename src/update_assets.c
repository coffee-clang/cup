/* Owns the fixed five-asset CUP update generation without owning update orchestration. */

#include "update_assets.h"

#include "assets.h"
#include "layout.h"
#include "text.h"

#include <string.h>

typedef struct {
    const char *new_name;
    const char *old_name;
    const char *absent_name;
    const char *generation_key;
    int executable;
    int read_only;
} UpdateAssetDefinition;

static const UpdateAssetDefinition definitions[CUP_UPDATE_ASSET_COUNT] = {
    {CUP_UPDATE_BINARY_NEW,
     CUP_UPDATE_BINARY_OLD,
     CUP_UPDATE_BINARY_ABSENT,
     "binary_sha256",
     1,
     0},
    {CUP_UPDATE_PLATFORM_CHECKSUMS_NEW,
     CUP_UPDATE_PLATFORM_CHECKSUMS_OLD,
     CUP_UPDATE_PLATFORM_CHECKSUMS_ABSENT,
     "platform_checksums_sha256",
     0,
     1},
    {CUP_UPDATE_PACKAGES_NEW,
     CUP_UPDATE_PACKAGES_OLD,
     CUP_UPDATE_PACKAGES_ABSENT,
     "packages_sha256",
     0,
     1},
    {CUP_UPDATE_INSTALL_POLICY_NEW,
     CUP_UPDATE_INSTALL_POLICY_OLD,
     CUP_UPDATE_INSTALL_POLICY_ABSENT,
     "install_policy_sha256",
     0,
     1},
    {CUP_UPDATE_COMMON_CHECKSUMS_NEW,
     CUP_UPDATE_COMMON_CHECKSUMS_OLD,
     CUP_UPDATE_COMMON_CHECKSUMS_ABSENT,
     "common_checksums_sha256",
     0,
     1},
};

CupError update_asset_spec(UpdateAssetId id, UpdateAssetSpec *spec) {
    const UpdateAssetDefinition *definition;
    if (spec == NULL || id < CUP_UPDATE_ASSET_BINARY || id >= CUP_UPDATE_ASSET_COUNT) {
        return CUP_ERR_INVALID_INPUT;
    }
    definition = &definitions[id];
    memset(spec, 0, sizeof(*spec));
    spec->new_name = definition->new_name;
    spec->old_name = definition->old_name;
    spec->absent_name = definition->absent_name;
    spec->generation_key = definition->generation_key;
    spec->executable = definition->executable;
    spec->read_only = definition->read_only;
    return CUP_OK;
}

CupError update_asset_specs(UpdateAssetSpec specs[CUP_UPDATE_ASSET_COUNT]) {
    size_t i;
    CupError err;

    if (specs == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; i < CUP_UPDATE_ASSET_COUNT; ++i) {
        err = update_asset_spec((UpdateAssetId)i, &specs[i]);
        if (err != CUP_OK) {
            return err;
        }
    }
    return CUP_OK;
}

CupError update_asset_destination(UpdateAssetId id, char *path, size_t size) {
    CupError err;

    if (path == NULL || size == 0 || id < CUP_UPDATE_ASSET_BINARY || id >= CUP_UPDATE_ASSET_COUNT) {
        return CUP_ERR_INVALID_INPUT;
    }
    switch (id) {
        case CUP_UPDATE_ASSET_BINARY:
            err = layout_get_binary_path(path, size);
            break;
        case CUP_UPDATE_ASSET_PLATFORM_CHECKSUMS:
            err = layout_get_platform_checksums_path(path, size);
            break;
        case CUP_UPDATE_ASSET_PACKAGES:
            err = layout_get_package_catalog_path(path, size);
            break;
        case CUP_UPDATE_ASSET_INSTALL_POLICY:
            err = layout_get_install_policy_path(path, size);
            break;
        case CUP_UPDATE_ASSET_COMMON_CHECKSUMS:
            err = layout_get_common_checksums_path(path, size);
            break;
        default:
            return CUP_ERR_INVALID_INPUT;
    }
    return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

CupError update_asset_release_name(UpdateAssetId id, char *name, size_t size) {
    if (name == NULL || size == 0 || id < CUP_UPDATE_ASSET_BINARY || id >= CUP_UPDATE_ASSET_COUNT) {
        return CUP_ERR_INVALID_INPUT;
    }
    switch (id) {
        case CUP_UPDATE_ASSET_BINARY:
            return assets_binary_asset_name(name, size);
        case CUP_UPDATE_ASSET_PLATFORM_CHECKSUMS:
            return assets_platform_checksums_name(name, size);
        case CUP_UPDATE_ASSET_PACKAGES:
            return text_copy(name, size, CUP_PACKAGES_FILENAME);
        case CUP_UPDATE_ASSET_INSTALL_POLICY:
            return text_copy(name, size, CUP_INSTALL_POLICY_FILENAME);
        case CUP_UPDATE_ASSET_COMMON_CHECKSUMS:
            return text_copy(name, size, CUP_COMMON_CHECKSUMS_FILENAME);
        default:
            return CUP_ERR_INVALID_INPUT;
    }
}
