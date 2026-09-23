#ifndef CUP_CONSTANTS_H
#define CUP_CONSTANTS_H

#include "domain_registry.h"

/* Central capacities, transfer/archive limits, and canonical cup asset filenames. */

/* Installed package history is dynamically represented. Defaults remain one-per
 * component/target scope because the host belongs to the authenticated root. */
#define MAX_STATE_DEFAULTS CUP_LOCAL_SCOPE_COUNT

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
#define CUP_STATE_FORMAT 2
#define MAX_STATE_LINE_LEN 256
#define MAX_STATE_FILE_BYTES MAX_PERSISTENT_METADATA_BYTES

/* Persistent root identity and deterministic fallback. */
#define CUP_PRIMARY_ROOT_DIRECTORY ".cup"
#define CUP_FALLBACK_ROOT_DIRECTORY ".coffee-cup"
#define CUP_ROOT_MARKER_FILENAME "root.txt"
#define CUP_ROOT_MARKER_FORMAT 2
#define CUP_ROOT_MARKER_PRODUCT "coffee-clang/cup"
#define CUP_ROOT_LAYOUT_FORMAT 2

/* Package catalog file. */
#define CUP_PACKAGE_CATALOG_FORMAT 1
#define MAX_CATALOG_LINE_LEN 1024
#define MAX_CATALOG_KEY_LEN 128
#define MAX_CATALOG_VALUE_LEN 512
#define MAX_CATALOG_URL_LEN 896

/* Scoped install defaults, profiles, toolchains and local preferences. */
#define MAX_INSTALL_POLICY_LINE_LEN 512
#define MAX_INSTALL_DEFAULTS CUP_GLOBAL_SCOPE_COUNT
#define MAX_TOOL_PREFERENCES CUP_LOCAL_SCOPE_COUNT
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
#define CUP_CATALOG_FILENAME "catalog.cfg"
#define CUP_INSTALL_POSIX_FILENAME "install.sh"
#define CUP_INSTALL_WINDOWS_FILENAME "install.ps1"
#define CUP_PREFERENCES_FILENAME "preferences.txt"
#define CUP_INFO_FILENAME "info.txt"
#define CUP_MANIFEST_FILENAME "manifest.txt"

/* Official release locations. */
#define CUP_RELEASE_LATEST_URL "https://github.com/coffee-clang/cup/releases/latest/download"
#define CUP_RELEASE_VERSIONED_URL_TEMPLATE \
    "https://github.com/coffee-clang/cup/releases/download/v%s"
#define CUP_RELEASE_METADATA_FILENAME "release.txt"

/* Shared detached update/uninstall protocol names. */
#define CUP_INSTALL_TEMP_PREFIX ".cup-install"
#define CUP_UPDATE_TEMP_PREFIX "cup-update"
#define CUP_UPDATE_NEW_DIRECTORY "new"
#define CUP_UPDATE_OLD_DIRECTORY "old"
#define CUP_UNINSTALL_TEMP_PREFIX ".cup-uninstall"
#define CUP_UPDATE_JOURNAL_OPERATION "cup-generation"
#define CUP_UNINSTALL_JOURNAL_OPERATION "uninstall"
#define CUP_INTERNAL_UPDATE_HELPER_ARGUMENT "--internal-update-helper"
#define CUP_INTERNAL_UNINSTALL_HELPER_ARGUMENT "--internal-uninstall-helper"

/* Platform-specific executable and derived update-helper name. */
#if defined(_WIN32)
#define CUP_BINARY_FILENAME "cup.exe"
#define CUP_UPDATE_HELPER_FILENAME "update-helper.exe"
#else
#define CUP_BINARY_FILENAME "cup"
#define CUP_UPDATE_HELPER_FILENAME "update-helper"
#endif

/* Development-only repository path. */

#endif /* CUP_CONSTANTS_H */
