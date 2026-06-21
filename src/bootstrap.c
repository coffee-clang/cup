#include "bootstrap.h"

#include "checksum.h"
#include "layout.h"
#include "manifest.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <string.h>

static CupError inspect_regular_file(const char *path,
    BootstrapAssetStatus *status) {
    SystemPathKind kind;
    CupError err;

    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        *status = BOOTSTRAP_ASSET_MISSING;
    } else if (kind == SYSTEM_PATH_REGULAR_FILE) {
        *status = BOOTSTRAP_ASSET_VALID;
    } else {
        *status = BOOTSTRAP_ASSET_INVALID;
    }
    return CUP_OK;
}

static CupError inspect_checksum_file(const char *path,
    const char *const *asset_names, size_t asset_count,
    BootstrapAssetStatus *status) {
    CupError err;

    err = inspect_regular_file(path, status);
    if (err != CUP_OK || *status != BOOTSTRAP_ASSET_VALID) {
        return err;
    }
    err = checksum_validate_assets(path, asset_names, asset_count);
    if (err == CUP_ERR_VALIDATION) {
        *status = BOOTSTRAP_ASSET_INVALID;
        return CUP_OK;
    }
    return err;
}

static CupError inspect_installed_assets(BootstrapInspection *inspection) {
    Manifest manifest;
    CupError err;
    char binary[MAX_PATH_LEN];
    char manifest_path[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char binary_asset[MAX_NAME_LEN];
    const char *common_assets[] = {CUP_MANIFEST_FILENAME};
    const char *platform_assets[2];
    int valid;

    if (layout_get_binary_path(binary, sizeof(binary)) != CUP_OK ||
        layout_get_manifest_path(manifest_path, sizeof(manifest_path)) != CUP_OK ||
        layout_get_uninstall_path(uninstall, sizeof(uninstall)) != CUP_OK ||
        layout_get_common_checksums_path(common_checksums,
            sizeof(common_checksums)) != CUP_OK ||
        layout_get_platform_checksums_path(platform_checksums,
            sizeof(platform_checksums)) != CUP_OK ||
        bootstrap_binary_asset_name(binary_asset, sizeof(binary_asset)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;

    err = inspect_checksum_file(common_checksums,
        common_assets, sizeof(common_assets) / sizeof(common_assets[0]),
        &inspection->common_checksums);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_checksum_file(platform_checksums,
        platform_assets, sizeof(platform_assets) / sizeof(platform_assets[0]),
        &inspection->platform_checksums);
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_regular_file(binary, &inspection->binary);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->binary == BOOTSTRAP_ASSET_VALID) {
        int executable;

        err = system_is_executable(binary, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable ||
            inspection->platform_checksums != BOOTSTRAP_ASSET_VALID) {
            inspection->binary = BOOTSTRAP_ASSET_INVALID;
        } else {
            err = bootstrap_verify_asset(platform_checksums, binary_asset,
                binary, &valid);
            if (err != CUP_OK) {
                return err;
            }
            if (!valid) {
                inspection->binary = BOOTSTRAP_ASSET_INVALID;
            }
        }
    }

    err = inspect_regular_file(manifest_path, &inspection->manifest);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->manifest == BOOTSTRAP_ASSET_VALID) {
        manifest_init(&manifest);
        err = manifest_load_installed(&manifest);
        manifest_free(&manifest);
        if (err != CUP_OK ||
            inspection->common_checksums != BOOTSTRAP_ASSET_VALID) {
            inspection->manifest = BOOTSTRAP_ASSET_INVALID;
        } else {
            err = bootstrap_verify_asset(common_checksums,
                CUP_MANIFEST_FILENAME, manifest_path, &valid);
            if (err != CUP_OK) {
                return err;
            }
            if (!valid) {
                inspection->manifest = BOOTSTRAP_ASSET_INVALID;
            }
        }
    }

    err = inspect_regular_file(uninstall, &inspection->uninstall);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection->uninstall == BOOTSTRAP_ASSET_VALID) {
#if !defined(_WIN32)
        int executable;

        err = system_is_executable(uninstall, &executable);
        if (err != CUP_OK) {
            return err;
        }
        if (!executable) {
            inspection->uninstall = BOOTSTRAP_ASSET_INVALID;
        }
#endif
        if (inspection->uninstall == BOOTSTRAP_ASSET_VALID) {
            if (inspection->platform_checksums != BOOTSTRAP_ASSET_VALID) {
                inspection->uninstall = BOOTSTRAP_ASSET_INVALID;
            } else {
                err = bootstrap_verify_asset(platform_checksums,
                    CUP_UNINSTALL_FILENAME, uninstall, &valid);
                if (err != CUP_OK) {
                    return err;
                }
                if (!valid) {
                    inspection->uninstall = BOOTSTRAP_ASSET_INVALID;
                }
            }
        }
    }

    return CUP_OK;
}

static CupError inspect_development_assets(BootstrapInspection *inspection) {
    Manifest manifest;
    CupError err;
    int regular;

    manifest_init(&manifest);
    inspection->development_manifest_valid =
        manifest_load_development(&manifest) == CUP_OK;
    manifest_free(&manifest);

    err = system_is_regular_file(CUP_DEVELOPMENT_UNINSTALL_PATH, &regular);
    if (err != CUP_OK) {
        return err;
    }
    inspection->development_uninstall_valid = regular;
#if !defined(_WIN32)
    if (regular) {
        int executable;

        err = system_is_executable(CUP_DEVELOPMENT_UNINSTALL_PATH,
            &executable);
        if (err != CUP_OK) {
            return err;
        }
        inspection->development_uninstall_valid = executable;
    }
#endif
    return CUP_OK;
}

CupError bootstrap_inspect(BootstrapInspection *inspection) {
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

int bootstrap_has_installed_assets(const BootstrapInspection *inspection) {
    if (inspection == NULL) {
        return 0;
    }
    return inspection->binary != BOOTSTRAP_ASSET_MISSING ||
        inspection->manifest != BOOTSTRAP_ASSET_MISSING ||
        inspection->uninstall != BOOTSTRAP_ASSET_MISSING ||
        inspection->common_checksums != BOOTSTRAP_ASSET_MISSING ||
        inspection->platform_checksums != BOOTSTRAP_ASSET_MISSING;
}

int bootstrap_installed_is_valid(const BootstrapInspection *inspection) {
    return inspection != NULL &&
        inspection->binary == BOOTSTRAP_ASSET_VALID &&
        inspection->manifest == BOOTSTRAP_ASSET_VALID &&
        inspection->uninstall == BOOTSTRAP_ASSET_VALID &&
        inspection->common_checksums == BOOTSTRAP_ASSET_VALID &&
        inspection->platform_checksums == BOOTSTRAP_ASSET_VALID;
}

int bootstrap_development_is_valid(const BootstrapInspection *inspection) {
    return inspection != NULL && inspection->development_manifest_valid &&
        inspection->development_uninstall_valid;
}

CupError bootstrap_find_uninstall(char *path, size_t size,
    BootstrapSource *source) {
    BootstrapInspection inspection;
    CupError err;

    if (path == NULL || size == 0 || source == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *source = BOOTSTRAP_SOURCE_NONE;

    err = bootstrap_inspect(&inspection);
    if (err != CUP_OK) {
        return err;
    }
    if (inspection.uninstall == BOOTSTRAP_ASSET_VALID) {
        *source = BOOTSTRAP_SOURCE_INSTALLED;
        return layout_get_uninstall_path(path, size);
    }
    if (inspection.development_uninstall_valid) {
        *source = BOOTSTRAP_SOURCE_DEVELOPMENT;
        return text_format(path, size, "%s", CUP_DEVELOPMENT_UNINSTALL_PATH);
    }
    return CUP_ERR_FILESYSTEM;
}

CupError bootstrap_uninstall_is_pending(int *pending) {
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

CupError bootstrap_binary_asset_name(char *name, size_t size) {
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

CupError bootstrap_platform_checksums_name(char *name, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err = platform_get_host(host, sizeof(host));

    if (err != CUP_OK) {
        return err;
    }
    return text_format(name, size, "SHA256SUMS.%s", host);
}

CupError bootstrap_verify_asset(const char *checksum_path,
    const char *asset_name, const char *asset_path, int *matches) {
    return checksum_verify_file(checksum_path, asset_name, asset_path, matches);
}
