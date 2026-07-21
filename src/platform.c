/*
 * Detects the native host identifier and validates the finite platform set used by package
 * catalogs, state and release assets.
 */

#include "platform.h"

#include "constants.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define MAX_ARCH_ENTRIES_PER_OS 4

/* Supported platforms. */
typedef struct {
    const char *os;
    const char *arch[MAX_ARCH_ENTRIES_PER_OS];
} SupportedPlatform;

static const SupportedPlatform SUPPORTED_PLATFORMS[] = {{"linux", {"x64", "arm64", NULL}},
                                                        {"windows", {"x64", NULL}},
                                                        {"macos", {"x64", "arm64", NULL}}};

/* Platform lookup. */
static const SupportedPlatform *find_supported_os(const char *os) {
    size_t count;
    size_t i;

    if (text_is_empty(os)) {
        return NULL;
    }

    count = sizeof(SUPPORTED_PLATFORMS) / sizeof(SUPPORTED_PLATFORMS[0]);

    for (i = 0; i < count; ++i) {
        if (strcmp(SUPPORTED_PLATFORMS[i].os, os) == 0) {
            return &SUPPORTED_PLATFORMS[i];
        }
    }

    return NULL;
}

/* Public API. */
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
    return err;
}

CupError platform_validate(const char *platform) {
    CupError err;
    const SupportedPlatform *supported;
    TextBuffer split_outputs[2];
    char platform_copy[MAX_PLATFORM_LEN];
    char os[MAX_IDENTIFIER_LEN];
    char arch[MAX_IDENTIFIER_LEN];
    size_t i;

    if (text_is_empty(platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_copy(platform_copy, sizeof(platform_copy), platform);
    if (err != CUP_OK) {
        return err;
    }

    split_outputs[0] = (TextBuffer){.data = os, .capacity = sizeof(os)};
    split_outputs[1] = (TextBuffer){.data = arch, .capacity = sizeof(arch)};

    err = text_split_exact(platform_copy, '-', split_outputs, 2);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: invalid platform '%s'. Expected format '<os>-<arch>'.\n", platform);
        return CUP_ERR_INVALID_INPUT;
    }

    supported = find_supported_os(os);
    if (supported == NULL) {
        fprintf(stderr, "Error: unsupported os '%s'.\n", os);
        return CUP_ERR_INVALID_OS;
    }

    for (i = 0; i < MAX_ARCH_ENTRIES_PER_OS && supported->arch[i] != NULL; ++i) {
        if (strcmp(supported->arch[i], arch) == 0) {
            return CUP_OK;
        }
    }

    fprintf(stderr, "Error: unsupported arch '%s' for os '%s'.\n", arch, os);
    return CUP_ERR_INVALID_ARCH;
}
