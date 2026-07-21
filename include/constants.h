#ifndef CUP_CONSTANTS_H
#define CUP_CONSTANTS_H

/*
 * Module contract: Central bounded capacities, transfer/archive limits and
 * canonical release and CUP asset filenames.
 */

/*
 * In-memory state capacity. One active package can exist for each component, host,
 * and target scope.
 */
#define MAX_INSTALLED 128
#define MAX_ACTIVE_PACKAGES 32

/* Generic buffer sizes. */
#define MAX_IDENTIFIER_LEN 32
#define MAX_SELECTOR_LEN 64
#define MAX_COMMAND_NAME_LEN 128
#define MAX_PLATFORM_LEN 64
#define MAX_PATH_LEN 1024

/* State file. */
#define CUP_STATE_FORMAT 1
#define MAX_STATE_LINE_LEN 256

/* PackageCatalog file. */
#define MAX_CATALOG_LINE_LEN 1024
#define MAX_CATALOG_KEY_LEN 128
#define MAX_CATALOG_VALUE_LEN 512
#define MAX_CATALOG_URL_LEN 896

/* Scoped install defaults, profiles, toolchains and local preferences. */
#define MAX_INSTALL_POLICY_LINE_LEN 512
#define MAX_INSTALL_DEFAULTS 64
#define MAX_INSTALL_PROFILES 8
#define MAX_INSTALL_TOOLCHAINS 8
#define MAX_INSTALL_SELECTIONS 16
#define MAX_INSTALL_LIST_ITEMS 16

/* Info file. */
#define MAX_METADATA_LINE_LEN 512
#define MAX_METADATA_VALUE_LEN 384
#define MAX_METADATA_KEY_LEN 128

/* Download limits. */
#define MAX_METADATA_DOWNLOAD_BYTES (4ULL * 1024ULL * 1024ULL)
#define MAX_BINARY_DOWNLOAD_BYTES (256ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_DOWNLOAD_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)

/* Package archives. */
/* Sized above current full toolchain packages while bounding memory and disk use. */
#define MAX_PACKAGE_ARCHIVE_ENTRIES 262144u
#define MAX_PACKAGE_ENTRY_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_EXTRACTED_BYTES (64ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_PATH_DEPTH 64u

/* Shared file names and development paths. */
#define CUP_PACKAGES_FILENAME "packages.cfg"
#define CUP_INSTALL_POLICY_FILENAME "install.cfg"
#define CUP_PREFERENCES_FILENAME "preferences.txt"
#define CUP_INFO_FILENAME "info.txt"
#define CUP_COMMON_CHECKSUMS_FILENAME "SHA256SUMS.common"
#define CUP_UNINSTALL_MARKER_FILENAME "uninstall.pending"
#define CUP_RELEASE_LATEST_URL "https://github.com/coffee-clang/cup/releases/latest/download"
#define CUP_RELEASE_VERSIONED_URL_TEMPLATE \
    "https://github.com/coffee-clang/cup/releases/download/v%s"
#define CUP_RELEASE_METADATA_FILENAME "release.txt"

#define CUP_UPDATE_BINARY_NEW "binary.new"
#define CUP_UPDATE_UNINSTALL_NEW "uninstall.new"
#define CUP_UPDATE_PLATFORM_CHECKSUMS_NEW "platform-checksums.new"
#define CUP_UPDATE_PACKAGES_NEW "manifest.new"
#define CUP_UPDATE_INSTALL_POLICY_NEW "install-config.new"
#define CUP_UPDATE_COMMON_CHECKSUMS_NEW "common-checksums.new"
#define CUP_UPDATE_BINARY_OLD "binary.old"
#define CUP_UPDATE_UNINSTALL_OLD "uninstall.old"
#define CUP_UPDATE_PLATFORM_CHECKSUMS_OLD "platform-checksums.old"
#define CUP_UPDATE_PACKAGES_OLD "manifest.old"
#define CUP_UPDATE_INSTALL_POLICY_OLD "install-config.old"
#define CUP_UPDATE_COMMON_CHECKSUMS_OLD "common-checksums.old"
#define CUP_UPDATE_COMMITTED "committed"
#define CUP_UPDATE_RESULT_FILENAME "cup-update-result.txt"
#define CUP_UPDATE_HELPER_FILENAME "cup-update-helper"

#if defined(_WIN32)
#define CUP_UNINSTALL_FILENAME "uninstall.ps1"
#define CUP_DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup-windows.ps1"
#else
#define CUP_UNINSTALL_FILENAME "uninstall.sh"
#define CUP_DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup.sh"
#endif

#define CUP_DEVELOPMENT_INSTALL_POLICY_PATH "config/install.cfg"

#endif /* CUP_CONSTANTS_H */
