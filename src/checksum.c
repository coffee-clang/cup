/* SHA-256 hashing primitives used by package, catalog and generation integrity boundaries. */

#include "checksum.h"

#include "third_party/sha256.h"

#include "interrupt.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(CHECKSUM_SHA256_HEX_LENGTH == SHA256_HEX_LENGTH,
               "checksum SHA-256 hex contract must match adapted backend");

static void format_digest(const unsigned char digest[SHA256_DIGEST_SIZE], char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0fu];
    }
    hex[SHA256_HEX_LENGTH] = '\0';
}

static CupError digest_stream(FILE *file, unsigned char digest[SHA256_DIGEST_SIZE]) {
    Sha256Context context;
    unsigned char buffer[8192];
    CupError err = CUP_OK;

    if (file == NULL || digest == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    sha256_init(&context);
    while (1) {
        size_t count;

        if (interrupt_requested()) {
            err = CUP_ERR_INTERRUPT;
            break;
        }
        errno = 0;
        count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) {
            sha256_update(&context, buffer, count);
        }
        if (count < sizeof(buffer)) {
            if (ferror(file)) {
                err = errno == EINTR && interrupt_requested() ? CUP_ERR_INTERRUPT
                                                              : CUP_ERR_FILESYSTEM;
            }
            break;
        }
    }

    if (err == CUP_OK && interrupt_requested()) {
        err = CUP_ERR_INTERRUPT;
    }
    if (err == CUP_OK) {
        sha256_final(&context, digest);
    }
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    return err;
}

CupError checksum_sha256_bytes(const unsigned char *data,
                               size_t data_size,
                               char *hex,
                               size_t size) {
    Sha256Context context;
    unsigned char digest[SHA256_DIGEST_SIZE];

    if (hex != NULL && size > 0) {
        hex[0] = '\0';
    }
    if ((data == NULL && data_size != 0) || hex == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    sha256_init(&context);
    if (data_size != 0) {
        sha256_update(&context, data, data_size);
    }
    sha256_final(&context, digest);
    format_digest(digest, hex);
    return CUP_OK;
}

CupError checksum_sha256_stream(FILE *file, char *hex, size_t size) {
    unsigned char digest[SHA256_DIGEST_SIZE];
    CupError err;

    if (hex != NULL && size > 0) {
        hex[0] = '\0';
    }
    if (file == NULL || hex == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    err = digest_stream(file, digest);
    if (err == CUP_OK) {
        format_digest(digest, hex);
    }
    return err;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    FILE *file = NULL;
    SystemPathIdentity identity;
    uint64_t file_size;
    int missing;
    CupError err;

    if (hex != NULL && size > 0) {
        hex[0] = '\0';
    }
    if (text_is_empty(path) || hex == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memset(&identity, 0, sizeof(identity));
    err = system_open_regular_file(path, &file, &identity, &file_size, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    (void)identity;
    (void)file_size;

    err = checksum_sha256_stream(file, hex, size);
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
        hex[0] = '\0';
    }
    return err;
}

int checksum_digest_is_canonical(const char *value) {
    size_t i;

    if (value == NULL || strlen(value) != SHA256_HEX_LENGTH) {
        return 0;
    }
    for (i = 0; i < SHA256_HEX_LENGTH; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}
