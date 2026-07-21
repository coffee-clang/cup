/*
 * Inspects the installed CUP assets and verifies canonical assets against the published checksum
 * files. Development fallbacks are reported separately and never treated as an installed CUP
 * asset generation.
 */

#include "cup_assets.h"

#include "checksum.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <string.h>

static CupError inspect_regular_file(const char *path, CupAssetStatus *status) {
    SystemPathKind kind;
    CupError err;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        *status = CUP_ASSET_MISSING;
    } else if (kind == SYSTEM_PATH_REGULAR_FILE) {
        *status = CUP_ASSET_VALID;
    } else {
        *status = CUP_ASSET_INVALID;
    }
    return CUP_OK;
}

static CupError inspect_checksum_file(const char *path,
                                      const char *const *asset_names,
                                      size_t asset_count,
                                      CupAssetStatus *status) {
    CupError err;

    err = inspect_regular_file(path, status);
    if (err != CUP_OK || *status != CUP_ASSET_VALID) {
        return err;
    }
    err = checksum_validate_assets(path, asset_names, asset_count);
    if (err == CUP_ERR_VALIDATION) {
        *status = CUP_ASSET_INVALID;
        return CUP_OK;
    }
    return err;
}

/* Installed-generation inspection. Every official asset is classified separately so doctor and
 * repair can report the exact failure. */
static CupError inspect_installed_assets(CupAssetsInspection *inspection) {
    PackageCatalog catalog;
    InstallPolicy install_policy;
    CupError err;
    char binary[MAX_PATH_LEN];
    char helper[MAX_PATH_LEN];
    char package_catalog_path[MAX_PATH_LEN];
    char install_policy_path[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
    const char *common_assets[] = {CUP_PACKAGES_FILENAME, CUP_INSTALL_POLICY_FILENAME};
    const char *platform_assets[3];
    int valid;

    err = layout_get_binary_path(binary, sizeof(binary));
    if (err == CUP_OK) {
        err = layout_get_cup_update_helper_path(helper, sizeof(helper));
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(package_catalog_path, sizeof(package_catalog_path));
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(install_policy_path, sizeof(install_policy_path));
    }
    if (err == CUP_OK) {
        err = layout_get_uninstall_path(uninstall, sizeof(uninstall));
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(common_checksums, sizeof(common_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(platform_checksums, sizeof(platform_checksums));
    }
    if (err == CUP_OK) {
        err = cup_assets_binary_asset_name(binary_asset, sizeof(binary_asset));
    }
    if (err != CUP_OK) {
        return err;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    platform_assets[2] = CUP_RELEASE_METADATA_FILENAME;

    err = inspect_checksum_file(common_checksums,
                                common_assets,
                                sizeof(common_assets) / sizeof(common_assets[0]),
                                &inspection->common_checksums);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_checksum_file(platform_checksums,
                                platform_assets,
                                sizeof(platform_assets) / sizeof(platform_assets[0]),
                                &inspection->platform_checksums);
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_regular_file(binary, &inspection->binary);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->binary == CUP_ASSET_VALID) {
        int executable;

        err = system_is_executable(binary, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable || inspection->platform_checksums != CUP_ASSET_VALID) {
            inspection->binary = CUP_ASSET_INVALID;
        } else {
            err = cup_assets_verify_asset(platform_checksums, binary_asset, binary, &valid);
            if (err != CUP_OK) {
                return err;
            }
            if (!valid) {
                inspection->binary = CUP_ASSET_INVALID;
            }
        }
    }

    err = inspect_regular_file(helper, &inspection->helper);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->helper == CUP_ASSET_VALID) {
#if !defined(_WIN32)
        int executable;

        err = system_is_executable(helper, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable) {
            inspection->helper = CUP_ASSET_INVALID;
        }
#endif
    }

    err = inspect_regular_file(package_catalog_path, &inspection->catalog);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->catalog == CUP_ASSET_VALID) {
        package_catalog_init(&catalog);
        err = package_catalog_load_installed(&catalog);
        package_catalog_free(&catalog);
        if (err == CUP_ERR_CATALOG || inspection->common_checksums != CUP_ASSET_VALID) {
            inspection->catalog = CUP_ASSET_INVALID;
        } else if (err != CUP_OK) {
            return err;
        } else {
            err = cup_assets_verify_asset(
                common_checksums, CUP_PACKAGES_FILENAME, package_catalog_path, &valid);
            if (err != CUP_OK) {
                return err;
            }
            if (!valid) {
                inspection->catalog = CUP_ASSET_INVALID;
            }
        }
    }

    err = inspect_regular_file(install_policy_path, &inspection->install_policy);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->install_policy == CUP_ASSET_VALID) {
        install_policy_init(&install_policy);
        err = install_policy_load_path(
            &install_policy, install_policy_path, INSTALL_POLICY_SOURCE_INSTALLED);
        if (err == CUP_ERR_VALIDATION || inspection->common_checksums != CUP_ASSET_VALID) {
            inspection->install_policy = CUP_ASSET_INVALID;
        } else if (err != CUP_OK) {
            return err;
        } else {
            err = cup_assets_verify_asset(
                common_checksums, CUP_INSTALL_POLICY_FILENAME, install_policy_path, &valid);
            if (err != CUP_OK) {
                return err;
            }
            if (!valid) {
                inspection->install_policy = CUP_ASSET_INVALID;
            }
        }
    }

    err = inspect_regular_file(uninstall, &inspection->uninstall);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->uninstall == CUP_ASSET_VALID) {
#if !defined(_WIN32)
        int executable;

        err = system_is_executable(uninstall, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable) {
            inspection->uninstall = CUP_ASSET_INVALID;
        }
#endif
        if (inspection->uninstall == CUP_ASSET_VALID) {
            if (inspection->platform_checksums != CUP_ASSET_VALID) {
                inspection->uninstall = CUP_ASSET_INVALID;
            } else {
                err = cup_assets_verify_asset(
                    platform_checksums, CUP_UNINSTALL_FILENAME, uninstall, &valid);
                if (err != CUP_OK) {
                    return err;
                }
                if (!valid) {
                    inspection->uninstall = CUP_ASSET_INVALID;
                }
            }
        }
    }

    return CUP_OK;
}

/* Development fallback inspection. Repository assets are accepted only when no official installed
 * generation is being claimed. */
static CupError inspect_development_assets(CupAssetsInspection *inspection) {
    PackageCatalog catalog;
    InstallPolicy install_policy;
    CupError err;
    int regular;

    package_catalog_init(&catalog);
    err = package_catalog_load_development(&catalog);
    package_catalog_free(&catalog);
    if (err == CUP_OK) {
        inspection->development_catalog_valid = 1;
    } else if (err != CUP_ERR_CATALOG) {
        return err;
    }

    install_policy_init(&install_policy);
    err = install_policy_load_development(&install_policy);
    if (err == CUP_OK) {
        inspection->development_install_policy_valid = 1;
    } else if (err != CUP_ERR_VALIDATION && err != CUP_ERR_FILESYSTEM) {
        return err;
    }

    err = system_is_regular_file(CUP_DEVELOPMENT_UNINSTALL_PATH, &regular);
    if (err != CUP_OK) {
        return err;
    }
    inspection->development_uninstall_valid = regular;
#if !defined(_WIN32)
    if (regular) {
        int executable;

        err = system_is_executable(CUP_DEVELOPMENT_UNINSTALL_PATH, &executable);
        if (err != CUP_OK) {
            return err;
        }
        inspection->development_uninstall_valid = executable;
    }
#endif
    return CUP_OK;
}

/* Public inspection and lookup API. These functions expose observations without performing repair
 * or download policy. */
CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    CupError err;

    if (inspection == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(inspection, 0, sizeof(*inspection));

    err = inspect_installed_assets(inspection);
    if (err != CUP_OK) {
        return err;
    }
    return inspect_development_assets(inspection);
}

int cup_assets_has_installed_assets(const CupAssetsInspection *inspection) {
    if (inspection == NULL) {
        return 0;
    }
    return inspection->binary != CUP_ASSET_MISSING || inspection->helper != CUP_ASSET_MISSING ||
           inspection->catalog != CUP_ASSET_MISSING ||
           inspection->install_policy != CUP_ASSET_MISSING ||
           inspection->uninstall != CUP_ASSET_MISSING ||
           inspection->common_checksums != CUP_ASSET_MISSING ||
           inspection->platform_checksums != CUP_ASSET_MISSING;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    return inspection != NULL && inspection->binary == CUP_ASSET_VALID &&
           inspection->helper == CUP_ASSET_VALID && inspection->catalog == CUP_ASSET_VALID &&
           inspection->install_policy == CUP_ASSET_VALID &&
           inspection->uninstall == CUP_ASSET_VALID &&
           inspection->common_checksums == CUP_ASSET_VALID &&
           inspection->platform_checksums == CUP_ASSET_VALID;
}

int cup_assets_development_is_valid(const CupAssetsInspection *inspection) {
    return inspection != NULL && inspection->development_catalog_valid &&
           inspection->development_install_policy_valid && inspection->development_uninstall_valid;
}

CupError cup_assets_find_uninstall(char *path, size_t size, CupAssetsSource *source) {
    CupAssetsInspection inspection;
    CupError err;

    if (path == NULL || size == 0 || source == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *source = CUP_ASSETS_SOURCE_NONE;

    err = cup_assets_inspect(&inspection);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection.uninstall == CUP_ASSET_VALID) {
        *source = CUP_ASSETS_SOURCE_INSTALLED;
        return layout_get_uninstall_path(path, size);
    }
    if (inspection.development_uninstall_valid) {
        *source = CUP_ASSETS_SOURCE_DEVELOPMENT;
        return text_copy(path, size, CUP_DEVELOPMENT_UNINSTALL_PATH);
    }
    return CUP_ERR_FILESYSTEM;
}

CupError cup_assets_uninstall_is_pending(int *pending) {
    SystemPathKind kind;
    CupError err;
    char marker[MAX_PATH_LEN];

    if (pending == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *pending = 0;

    err = layout_get_uninstall_marker_path(marker, sizeof(marker));
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(marker, &kind);
    if (err != CUP_OK) {
        return err;
    }

    *pending = kind != SYSTEM_PATH_MISSING;
    return CUP_OK;
}

CupError cup_assets_binary_asset_name(char *name, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err;

    err = platform_get_host(host, sizeof(host));
    if (err != CUP_OK) {
        return err;
    }
    if (strcmp(host, "windows-x64") == 0) {
        return text_format(name, size, "cup-%s.exe", host);
    }
    return text_format(name, size, "cup-%s", host);
}

CupError cup_assets_platform_checksums_name(char *name, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err = platform_get_host(host, sizeof(host));

    if (err != CUP_OK) {
        return err;
    }
    return text_format(name, size, "SHA256SUMS.%s", host);
}

CupError cup_assets_verify_asset(const char *checksum_path,
                                 const char *asset_name,
                                 const char *asset_path,
                                 int *matches) {
    return checksum_verify_file(checksum_path, asset_name, asset_path, matches);
}
