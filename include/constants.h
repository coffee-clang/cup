#ifndef CUP_CONSTANTS_H
#define CUP_CONSTANTS_H

// IN-MEMORY STATE CAPACITY
// One default can exist for each component, host and target scope.
#define MAX_INSTALLED 128
#define MAX_DEFAULTS 32

// GENERIC BUFFER SIZES
#define MAX_NAME_LEN 32
#define MAX_ENTRY_LEN 64
#define MAX_PLATFORM_LEN 64
#define MAX_PATH_LEN 1024

// STATE FILE
#define MAX_STATE_LINE_LEN 256

// MANIFEST FILE
#define MAX_MANIFEST_LINE_LEN 1024
#define MAX_MANIFEST_KEY_LEN 128
#define MAX_MANIFEST_VALUE_LEN 512
#define MAX_MANIFEST_URL_LEN 896

// INFO FILE
#define MAX_INFO_LINE_LEN 512
#define MAX_INFO_VALUE_LEN 384
#define MAX_INFO_KEY_LEN 128

// SHARED FILE NAMES AND DEVELOPMENT PATHS
#define CUP_MANIFEST_FILENAME "packages.cfg"
#define CUP_INFO_FILENAME "info.txt"

#if defined(_WIN32)
#define CUP_UNINSTALL_FILENAME "uninstall.ps1"
#define CUP_DEVELOPMENT_UNINSTALL_PATH \
    "scripts/install/uninstall-cup-windows.ps1"
#else
#define CUP_UNINSTALL_FILENAME "uninstall.sh"
#define CUP_DEVELOPMENT_UNINSTALL_PATH \
    "scripts/install/uninstall-cup.sh"
#endif

#endif /* CUP_CONSTANTS_H */
