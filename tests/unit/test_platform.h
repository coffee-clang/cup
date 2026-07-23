#ifndef CUP_TEST_PLATFORM_H
#define CUP_TEST_PLATFORM_H

/* Test-only compatibility for filesystem APIs that differ across hosts. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CUP_TEST_TEMP_PATH_SIZE 1024

#ifdef _WIN32
#include <direct.h>

static inline int test_mkdir(const char *path, int mode) {
    (void)mode;
    return _mkdir(path);
}

static inline char *test_make_temp_directory(char *buffer,
                                             size_t size,
                                             const char *name) {
    const char *base = getenv("RUNNER_TEMP");
    int written;

    if (base == NULL || base[0] == '\0') {
        base = getenv("TEMP");
    }
    if (base == NULL || base[0] == '\0') {
        base = getenv("TMP");
    }
    if (base == NULL || base[0] == '\0') {
        base = ".";
    }

    written = snprintf(buffer, size, "%s/%s-XXXXXX", base, name);
    if (written < 0 || (size_t)written >= size) {
        return NULL;
    }
    if (_mktemp_s(buffer, size) != 0 || _mkdir(buffer) != 0) {
        return NULL;
    }
    return buffer;
}
#else
#include <sys/stat.h>

static inline int test_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}

static inline char *test_make_temp_directory(char *buffer,
                                             size_t size,
                                             const char *name) {
    const char *base = getenv("TMPDIR");
    int written;

    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    written = snprintf(buffer, size, "%s/%s-XXXXXX", base, name);
    if (written < 0 || (size_t)written >= size) {
        return NULL;
    }
    return mkdtemp(buffer);
}
#endif

#endif /* CUP_TEST_PLATFORM_H */
