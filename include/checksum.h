#ifndef CUP_CHECKSUM_H
#define CUP_CHECKSUM_H

/* SHA-256 hashing and canonical digest validation for integrity boundaries. */

#include <stddef.h>
#include <stdio.h>

#include "error.h"

#define CHECKSUM_SHA256_HEX_LENGTH 64u

/* Validate one canonical lowercase hexadecimal SHA-256 digest. */
int checksum_digest_is_canonical(const char *value);

/*
 * Hash bytes, one open regular stream, or one regular path into lowercase
 * hexadecimal; short output storage is BUFFER_TOO_SMALL. Streams are rewound
 * before and after hashing.
 */
CupError checksum_sha256_bytes(const unsigned char *data, size_t data_size, char *hex, size_t size);
CupError checksum_sha256_stream(FILE *file, char *hex, size_t size);
CupError checksum_sha256_file(const char *path, char *hex, size_t size);

#endif /* CUP_CHECKSUM_H */
