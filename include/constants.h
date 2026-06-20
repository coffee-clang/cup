#ifndef CUP_CONSTANTS_H
#define CUP_CONSTANTS_H

// IN-MEMORY STATE CAPACITY
// The state keeps a bounded list of installed entries and one default entry
// for each logical component group.
#define MAX_INSTALLED 128
#define MAX_DEFAULTS 32

// GENERIC BUFFER SIZES
// Fixed-size buffers used across the C implementation.
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

// URL templates can be much longer than ordinary manifest values.
// This limit is derived from the manifest line size while preserving
// room for the maximum key length.
#define MAX_MANIFEST_URL_LEN 896

// INFO FILE
#define MAX_INFO_LINE_LEN 512
#define MAX_INFO_VALUE_LEN 384
#define MAX_INFO_KEY_LEN 128

#endif /* CUP_CONSTANTS_H */
