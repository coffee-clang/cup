/*
 * Detects the native host identifier and validates the finite platform set used by package
 * catalogs, state and release assets.
 */

#include "platform.h"

#include "constants.h"
#include "domain_registry.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define PLATFORM_ENTRY(os, arch) os "-" arch,
static const char *const SUPPORTED_PLATFORMS[] = {
    CUP_PLATFORM_REGISTRY(PLATFORM_ENTRY)
};
#undef PLATFORM_ENTRY

_Static_assert(sizeof(SUPPORTED_PLATFORMS) / sizeof(SUPPORTED_PLATFORMS[0]) ==
                   CUP_PLATFORM_COUNT,
               "platform registry count must remain derived");

int platform_is_supported(const char *platform) {
    size_t i;

    if (text_is_empty(platform) || strlen(platform) >= MAX_PLATFORM_LEN) {
        return 0;
    }
    for (i = 0; i < CUP_PLATFORM_COUNT; ++i) {
        if (strcmp(SUPPORTED_PLATFORMS[i], platform) == 0) {
            return 1;
        }
    }
    return 0;
}

CupError platform_validate(const char *platform) {
    TextBuffer parts[2];
    char copy[MAX_PLATFORM_LEN];
    char os[MAX_IDENTIFIER_LEN];
    char arch[MAX_IDENTIFIER_LEN];
    int os_known = 0;

    if (text_is_empty(platform)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strlen(platform) >= MAX_PLATFORM_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (platform_is_supported(platform)) {
        return CUP_OK;
    }

    if (text_copy(copy, sizeof(copy), platform) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    parts[0] = (TextBuffer){.data = os, .capacity = sizeof(os)};
    parts[1] = (TextBuffer){.data = arch, .capacity = sizeof(arch)};
    if (text_split_exact(copy, '-', parts, 2) != CUP_OK) {
        fprintf(stderr, "Error: invalid platform '%s'. Expected format '<os>-<arch>'.\n", platform);
        return CUP_ERR_INVALID_INPUT;
    }

#define MATCH_OS(entry_os, entry_arch) \
    if (strcmp(os, entry_os) == 0) {   \
        os_known = 1;                   \
    }
    CUP_PLATFORM_REGISTRY(MATCH_OS)
#undef MATCH_OS

    if (!os_known) {
        fprintf(stderr, "Error: unsupported os '%s'.\n", os);
        return CUP_ERR_INVALID_OS;
    }

    fprintf(stderr, "Error: unsupported arch '%s' for os '%s'.\n", arch, os);
    return CUP_ERR_INVALID_ARCH;
}

CupError platform_get_host(char *buffer, size_t size) {
    CupError err;
    const char *os;
    const char *arch;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

#if defined(_WIN32)
    os = "windows";
#elif defined(__linux__)
    os = "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    os = "macos";
#else
    return CUP_ERR_INVALID_OS;
#endif

#if defined(__x86_64__) || defined(_M_X64)
    arch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "arm64";
#else
    return CUP_ERR_INVALID_ARCH;
#endif

    err = text_format(buffer, size, "%s-%s", os, arch);
    if (err != CUP_OK) {
        return err;
    }

    /* Architecture detection must never manufacture an unsupported host. */
    return platform_validate(buffer);
}
