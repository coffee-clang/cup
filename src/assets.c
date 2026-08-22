/*
 * Inspects the installed cup assets and verifies canonical assets against the published checksum
 * files. Development fallbacks are reported separately and never treated as an installed cup
 * asset generation.
 */

#include "assets.h"

#include "checksum.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "platform.h"
#include "path.h"
#include "system.h"
#include "text.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static CupError inspect_regular_file(const char *path, AssetStatus *status) {
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

static CupError inspect_checksum_document(const char *path,
                                          const char *const *asset_names,
                                          size_t asset_count,
                                          AssetStatus *status,
                                          ChecksumDocument *document) {
    CupError err;

    err = inspect_regular_file(path, status);
    if (err != CUP_OK || *status != CUP_ASSET_VALID) {
        return err;
    }
    err = checksum_document_load(document, path);
    if (err == CUP_ERR_VALIDATION) {
        *status = CUP_ASSET_INVALID;
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }
    err = checksum_document_validate_assets(document, asset_names, asset_count);
    if (err == CUP_ERR_VALIDATION) {
        *status = CUP_ASSET_INVALID;
        return CUP_OK;
    }
    return err;
}

/* Installed-generation inspection. Every official asset is classified separately so doctor and
 * repair can report the exact failure. */
typedef struct {
    char binary[MAX_PATH_LEN];
    char helper[MAX_PATH_LEN];
    char package_catalog[MAX_PATH_LEN];
    char install_policy[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
} InstalledAssetPaths;

static CupError resolve_installed_asset_paths(InstalledAssetPaths *paths) {
    CupError err;

    err = layout_get_binary_path(paths->binary, sizeof(paths->binary));
    if (err == CUP_OK) {
        err = layout_get_update_helper_path(paths->helper, sizeof(paths->helper));
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(paths->package_catalog,
                                              sizeof(paths->package_catalog));
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(paths->install_policy,
                                             sizeof(paths->install_policy));
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(paths->common_checksums,
                                               sizeof(paths->common_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(paths->platform_checksums,
                                                 sizeof(paths->platform_checksums));
    }
    if (err == CUP_OK) {
        err = assets_binary_asset_name(paths->binary_asset, sizeof(paths->binary_asset));
    }
    return err;
}

static CupError inspect_binary_asset(const InstalledAssetPaths *paths,
                                     const ChecksumDocument *platform_checksums,
                                     AssetsInspection *inspection) {
    CupError err;
    int executable;
    int valid;

    err = inspect_regular_file(paths->binary, &inspection->binary);
    if (err != CUP_OK || inspection->binary != CUP_ASSET_VALID) {
        return err;
    }

    err = system_is_executable(paths->binary, &executable);
    if (err != CUP_OK) {
        return err;
    }
    if (!executable || inspection->platform_checksums != CUP_ASSET_VALID) {
        inspection->binary = CUP_ASSET_INVALID;
        return CUP_OK;
    }

    err = checksum_document_verify_file(
        platform_checksums, paths->binary_asset, paths->binary, &valid);
    if (err == CUP_OK && !valid) {
        inspection->binary = CUP_ASSET_INVALID;
    }
    return err;
}

static CupError inspect_update_helper_asset(const InstalledAssetPaths *paths,
                                            AssetsInspection *inspection) {
    CupError err;
    int executable;

    /* The update helper is derived from the canonical executable immediately before use. A stale
     * but executable regular copy is therefore harmless and must not invalidate the installed
     * generation after a successful cup update. */
    err = inspect_regular_file(paths->helper, &inspection->helper);
    if (err != CUP_OK || inspection->helper != CUP_ASSET_VALID) {
        return err;
    }

    err = system_is_executable(paths->helper, &executable);
    if (err != CUP_OK) {
        return err;
    }
    if (!executable) {
        inspection->helper = CUP_ASSET_INVALID;
    }
    return CUP_OK;
}

static CupError inspect_catalog_asset(const InstalledAssetPaths *paths,
                                      const ChecksumDocument *common_checksums,
                                      AssetsInspection *inspection) {
    PackageCatalog catalog;
    CupError err;
    int valid;

    err = inspect_regular_file(paths->package_catalog, &inspection->catalog);
    if (err != CUP_OK || inspection->catalog != CUP_ASSET_VALID) {
        return err;
    }

    package_catalog_init(&catalog);
    err = package_catalog_load_installed(&catalog);
    package_catalog_free(&catalog);
    if (err == CUP_ERR_CATALOG || inspection->common_checksums != CUP_ASSET_VALID) {
        inspection->catalog = CUP_ASSET_INVALID;
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }

    err = checksum_document_verify_file(
        common_checksums, CUP_PACKAGES_FILENAME, paths->package_catalog, &valid);
    if (err == CUP_OK && !valid) {
        inspection->catalog = CUP_ASSET_INVALID;
    }
    return err;
}

static CupError inspect_install_policy_asset(const InstalledAssetPaths *paths,
                                             const ChecksumDocument *common_checksums,
                                             AssetsInspection *inspection) {
    InstallPolicy install_policy;
    CupError err;
    int valid;

    err = inspect_regular_file(paths->install_policy, &inspection->install_policy);
    if (err != CUP_OK || inspection->install_policy != CUP_ASSET_VALID) {
        return err;
    }

    install_policy_init(&install_policy);
    err = install_policy_load_path(&install_policy, paths->install_policy);
    if (err == CUP_ERR_VALIDATION || inspection->common_checksums != CUP_ASSET_VALID) {
        inspection->install_policy = CUP_ASSET_INVALID;
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }

    err = checksum_document_verify_file(common_checksums,
                                        CUP_INSTALL_POLICY_FILENAME,
                                        paths->install_policy,
                                        &valid);
    if (err == CUP_OK && !valid) {
        inspection->install_policy = CUP_ASSET_INVALID;
    }
    return err;
}

static CupError inspect_installed_assets(AssetsInspection *inspection) {
    InstalledAssetPaths paths;
    ChecksumDocument common_document;
    ChecksumDocument platform_document;
    CupError err;
    const char *platform_assets[CUP_PLATFORM_CHECKSUM_ASSET_COUNT];

    checksum_document_init(&common_document);
    checksum_document_init(&platform_document);

    /* Resolve the complete installed generation before inspecting any individual asset. */
    err = resolve_installed_asset_paths(&paths);
    if (err != CUP_OK) {
        goto done;
    }

    platform_assets[0] = paths.binary_asset;
    platform_assets[1] = CUP_RELEASE_METADATA_FILENAME;
    platform_assets[2] = CUP_COMMON_CHECKSUMS_FILENAME;

    err = inspect_checksum_document(paths.common_checksums,
                                    CUP_COMMON_CHECKSUM_ASSETS,
                                    CUP_COMMON_CHECKSUM_ASSET_COUNT,
                                    &inspection->common_checksums,
                                    &common_document);
    if (err == CUP_OK) {
        err = inspect_checksum_document(paths.platform_checksums,
                                        platform_assets,
                                        sizeof(platform_assets) / sizeof(platform_assets[0]),
                                        &inspection->platform_checksums,
                                        &platform_document);
    }
    if (err != CUP_OK) {
        goto done;
    }

    /* The platform document authenticates the exact common document used below. */
    if (inspection->common_checksums == CUP_ASSET_VALID &&
        inspection->platform_checksums == CUP_ASSET_VALID) {
        int valid;

        err = checksum_document_verify_file(&platform_document,
                                            CUP_COMMON_CHECKSUMS_FILENAME,
                                            paths.common_checksums,
                                            &valid);
        if (err == CUP_OK && !valid) {
            inspection->common_checksums = CUP_ASSET_INVALID;
            inspection->catalog = CUP_ASSET_INVALID;
            inspection->install_policy = CUP_ASSET_INVALID;
        }
    }
    if (err != CUP_OK) {
        goto done;
    }

    err = inspect_binary_asset(&paths, &platform_document, inspection);
    if (err == CUP_OK) {
        err = inspect_update_helper_asset(&paths, inspection);
    }
    if (err == CUP_OK) {
        err = inspect_catalog_asset(&paths, &common_document, inspection);
    }
    if (err == CUP_OK) {
        err = inspect_install_policy_asset(&paths, &common_document, inspection);
    }

    /* Reject a mixed observation if either checksum pathname changed during inspection. */
    if (err == CUP_OK && inspection->common_checksums == CUP_ASSET_VALID) {
        SystemPathIdentity current;
        err = system_get_path_identity(paths.common_checksums, &current);
        if (err == CUP_OK && !system_path_identity_equal(&common_document.identity, &current)) {
            inspection->common_checksums = CUP_ASSET_INVALID;
            inspection->catalog = CUP_ASSET_INVALID;
            inspection->install_policy = CUP_ASSET_INVALID;
        }
    }
    if (err == CUP_OK && inspection->platform_checksums == CUP_ASSET_VALID) {
        SystemPathIdentity current;
        err = system_get_path_identity(paths.platform_checksums, &current);
        if (err == CUP_OK && !system_path_identity_equal(&platform_document.identity, &current)) {
            inspection->platform_checksums = CUP_ASSET_INVALID;
            inspection->binary = CUP_ASSET_INVALID;
            inspection->common_checksums = CUP_ASSET_INVALID;
            inspection->catalog = CUP_ASSET_INVALID;
            inspection->install_policy = CUP_ASSET_INVALID;
        }
    }

done:
    checksum_document_free(&common_document);
    checksum_document_free(&platform_document);
    return err;
}

#if !CUP_VERSION_OFFICIAL
/* Development fallback inspection. Repository assets are accepted only when no official installed
 * generation is being claimed. */
static CupError inspect_development_assets(AssetsInspection *inspection) {
    PackageCatalog catalog;
    InstallPolicy install_policy;
    CupError err;

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
    return CUP_OK;
}
#endif

/* Public inspection and lookup API. These functions expose observations without performing repair
 * or download policy. */
CupError assets_inspect(AssetsInspection *inspection) {
    CupError err;

    if (inspection == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(inspection, 0, sizeof(*inspection));

    err = inspect_installed_assets(inspection);
    if (err != CUP_OK) {
        return err;
    }
#if CUP_VERSION_OFFICIAL
    return CUP_OK;
#else
    return inspect_development_assets(inspection);
#endif
}

int assets_has_installed_assets(const AssetsInspection *inspection) {
    if (inspection == NULL) {
        return 0;
    }
    return inspection->binary != CUP_ASSET_MISSING ||
           inspection->catalog != CUP_ASSET_MISSING ||
           inspection->install_policy != CUP_ASSET_MISSING ||
           inspection->common_checksums != CUP_ASSET_MISSING ||
           inspection->platform_checksums != CUP_ASSET_MISSING;
}

int assets_installed_is_valid(const AssetsInspection *inspection) {
    return inspection != NULL && inspection->binary == CUP_ASSET_VALID &&
           inspection->catalog == CUP_ASSET_VALID &&
           inspection->install_policy == CUP_ASSET_VALID &&
           inspection->common_checksums == CUP_ASSET_VALID &&
           inspection->platform_checksums == CUP_ASSET_VALID;
}

int assets_development_is_valid(const AssetsInspection *inspection) {
    return inspection != NULL && inspection->development_catalog_valid &&
           inspection->development_install_policy_valid;
}

CupError assets_binary_asset_name(char *name, size_t size) {
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

CupError assets_platform_checksums_name(char *name, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err = platform_get_host(host, sizeof(host));

    if (err != CUP_OK) {
        return err;
    }
    return text_format(name, size, "SHA256SUMS.%s", host);
}
