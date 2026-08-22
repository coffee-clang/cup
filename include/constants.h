#ifndef CUP_CONSTANTS_H
#define CUP_CONSTANTS_H

#include "domain_registry.h"

/* Central capacities, transfer/archive limits, and canonical cup asset filenames. */

/*
 * Bounded in-memory state. Several concrete versions may coexist for one tool,
 * host and target, so installed-package capacity is an explicit resource limit.
 * At most one default exists for each component, host and target.
 */
#define MAX_INSTALLED 256
#define MAX_STATE_DEFAULTS CUP_GLOBAL_SCOPE_COUNT

/* Generic buffer sizes. */
#define MAX_IDENTIFIER_LEN 32
#define MAX_SELECTOR_LEN 64
#define MAX_COMMAND_NAME_LEN 128
#define MAX_PLATFORM_LEN 64
#define MAX_PUBLIC_COMMAND_NAME_LEN \
    (MAX_PLATFORM_LEN + 1 + MAX_COMMAND_NAME_LEN + sizeof(".cmd"))
#define MAX_PATH_SEGMENT_LEN 256
#define MAX_PATH_LEN 1024

/* State file. */
#define CUP_STATE_FORMAT 1
#define MAX_STATE_LINE_LEN 256
#define MAX_STATE_FILE_BYTES ((1u + MAX_INSTALLED + MAX_STATE_DEFAULTS) * MAX_STATE_LINE_LEN)

/* Persistent root identity and deterministic fallback. */
#define CUP_PRIMARY_ROOT_DIRECTORY ".cup"
#define CUP_FALLBACK_ROOT_DIRECTORY ".coffee-cup"
#define CUP_ROOT_MARKER_FILENAME "root.txt"
#define CUP_ROOT_MARKER_FORMAT 1
#define CUP_ROOT_MARKER_PRODUCT "coffee-clang/cup"
#define CUP_ROOT_LAYOUT_FORMAT 1

/* Package catalog file. */
#define CUP_PACKAGE_CATALOG_FORMAT 1
#define MAX_CATALOG_LINE_LEN 1024
#define MAX_CATALOG_KEY_LEN 128
#define MAX_CATALOG_VALUE_LEN 512
#define MAX_CATALOG_URL_LEN 896

/* Scoped install defaults, profiles, toolchains and local preferences. */
#define MAX_INSTALL_POLICY_LINE_LEN 512
#define MAX_INSTALL_DEFAULTS CUP_GLOBAL_SCOPE_COUNT
#define MAX_INSTALL_PROFILES 8
#define MAX_INSTALL_TOOLCHAINS 8
#define MAX_INSTALL_LIST_ITEMS 16

/* Shared persistent text and journal limits. */
#define MAX_METADATA_LINE_LEN 512
#define MAX_PERSISTENT_METADATA_BYTES (4u * 1024u * 1024u)
#define MAX_RUNTIME_JOURNAL_BYTES 8192u
#define MAX_TRANSACTION_TOKEN_LEN 256
#define MAX_METADATA_VALUE_LEN 384
#define MAX_METADATA_KEY_LEN 128

/* Download limits. */
#define MAX_METADATA_DOWNLOAD_BYTES (4ULL * 1024ULL * 1024ULL)
#define MAX_BINARY_DOWNLOAD_BYTES (256ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_DOWNLOAD_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)

/* Package archive limits, sized above current full toolchain packages. */
#define MAX_PACKAGE_ARCHIVE_ENTRIES 262144u
#define MAX_PACKAGE_ENTRY_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_EXTRACTED_BYTES (64ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_PACKAGE_PATH_DEPTH 64u
#define MAX_PACKAGE_PATH_TABLE_BYTES (256ULL * 1024ULL * 1024ULL)

/* Canonical installed asset filenames. */
#define CUP_PACKAGES_FILENAME "packages.cfg"
#define CUP_INSTALL_POLICY_FILENAME "install.cfg"
#define CUP_INSTALL_POSIX_FILENAME "install.sh"
#define CUP_INSTALL_WINDOWS_FILENAME "install.ps1"
#define CUP_PREFERENCES_FILENAME "preferences.txt"
#define CUP_INFO_FILENAME "info.txt"
#define CUP_COMMON_CHECKSUMS_FILENAME "SHA256SUMS.common"
#define CUP_COMMON_CHECKSUM_ASSET_COUNT 4u
#define CUP_PLATFORM_CHECKSUM_ASSET_COUNT 3u
#define CUP_COMMON_CHECKSUM_ASSETS \
    ((const char *const[]){CUP_PACKAGES_FILENAME, \
                           CUP_INSTALL_POLICY_FILENAME, \
                           CUP_INSTALL_POSIX_FILENAME, \
                           CUP_INSTALL_WINDOWS_FILENAME})

/* Official release locations. */
#define CUP_RELEASE_LATEST_URL "https://github.com/coffee-clang/cup/releases/latest/download"
#define CUP_RELEASE_VERSIONED_URL_TEMPLATE \
    "https://github.com/coffee-clang/cup/releases/download/v%s"
#define CUP_RELEASE_METADATA_FILENAME "release.txt"

/* cup update staging, backup, absence, and commit names. */
#define CUP_UPDATE_BINARY_NEW "binary.new"
#define CUP_UPDATE_PLATFORM_CHECKSUMS_NEW "platform-checksums.new"
#define CUP_UPDATE_PACKAGES_NEW "package-catalog.new"
#define CUP_UPDATE_INSTALL_POLICY_NEW "install-config.new"
#define CUP_UPDATE_COMMON_CHECKSUMS_NEW "common-checksums.new"
#define CUP_UPDATE_BINARY_OLD "binary.old"
#define CUP_UPDATE_PLATFORM_CHECKSUMS_OLD "platform-checksums.old"
#define CUP_UPDATE_PACKAGES_OLD "package-catalog.old"
#define CUP_UPDATE_INSTALL_POLICY_OLD "install-config.old"
#define CUP_UPDATE_COMMON_CHECKSUMS_OLD "common-checksums.old"
#define CUP_UPDATE_BINARY_ABSENT "binary.absent"
#define CUP_UPDATE_PLATFORM_CHECKSUMS_ABSENT "platform-checksums.absent"
#define CUP_UPDATE_PACKAGES_ABSENT "package-catalog.absent"
#define CUP_UPDATE_INSTALL_POLICY_ABSENT "install-config.absent"
#define CUP_UPDATE_COMMON_CHECKSUMS_ABSENT "common-checksums.absent"
#define CUP_UPDATE_COMMITTED "committed"

/* Platform-specific executable and persistent update-helper name. */
#if defined(_WIN32)
#define CUP_BINARY_FILENAME "cup.exe"
#define CUP_UPDATE_HELPER_FILENAME "update-helper.exe"
#else
#define CUP_BINARY_FILENAME "cup"
#define CUP_UPDATE_HELPER_FILENAME "update-helper"
#endif

/* Development-only repository path. */
#define CUP_DEVELOPMENT_INSTALL_POLICY_PATH "config/install.cfg"

#endif /* CUP_CONSTANTS_H */
