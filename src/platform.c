#include "platform.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define MAX_ARCH_PER_OS 4

typedef struct {
    const char *os;
    const char *arch[MAX_ARCH_PER_OS];
} SupportedPlatform;

static const SupportedPlatform SUPPORTED_PLATFORM[] = {
    { "linux", { "x64", "arm64", NULL } },
    { "windows", { "x64", "arm64", NULL } },
    { "macos", { "x64", "arm64", NULL } }
};

static const SupportedPlatform *find_supported_os(const char *os) {
    size_t count;
    size_t i;

    if (os == NULL || os[0] == '\0') {
        return NULL;
    }

    count = sizeof(SUPPORTED_PLATFORM) / sizeof(SUPPORTED_PLATFORM[0]);

    for (i = 0; i < count; ++i) {
        if (strcmp(SUPPORTED_PLATFORM[i].os, os) == 0) {
            return &SUPPORTED_PLATFORM[i];
        }
    }

    return NULL;
}

static CupError split_platform(const char *platform, char *os, size_t os_size, char *arch, size_t arch_size) {
    const char *at;
    size_t os_len;
    size_t arch_len;

    if (platform == NULL || os == NULL || arch == NULL ||
        platform[0] == '\0' || os_size == 0 || arch_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    at = strchr(platform, '-');
    if (at == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    os_len = (size_t)(at - platform);
    arch_len = strlen(at + 1);

    if (os_len == 0 || arch_len == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (os_len >= os_size || arch_len >= arch_size) {
        return CUP_ERR_INVALID_INPUT;
    }

    memcpy(os, platform, os_len);
    os[os_len] = '\0';

    memcpy(arch, at + 1, arch_len);
    arch[arch_len] = '\0';

    return CUP_OK;
}

CupError get_host_platform(char *buffer, size_t size) {
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

    err = checked_snprintf(buffer, size, "%s-%s", os, arch);
    return err;
}

CupError validate_platform(const char *platform) {
    CupError err;
    const SupportedPlatform *supported;
    char os[MAX_NAME_LEN];
    char arch[MAX_NAME_LEN];
    size_t i;

    if (platform == NULL || platform[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_platform(platform, os, sizeof(os), arch, sizeof(arch));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: invalid platform '%s'. Expected format '<os>-<arch>'.\n", platform);
        return err;
    }

    supported = find_supported_os(os);
    if (supported == NULL) {
        fprintf(stderr, "Error: unsupported os '%s'.\n", os);
        return CUP_ERR_INVALID_OS;
    }

    for (i = 0; i < MAX_ARCH_PER_OS && supported->arch[i] != NULL; ++i) {
        if (strcmp(supported->arch[i], arch) == 0) {
            return CUP_OK;
        }
    }

    fprintf(stderr, "Error: unsupported arch '%s' for os '%s'.\n", arch, os);
    return CUP_ERR_INVALID_ARCH;
}