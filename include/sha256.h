#ifndef CUP_SHA256_H
#define CUP_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32u

typedef struct {
    uint8_t data[64];
    size_t data_length;
    uint64_t bit_length;
    uint32_t state[8];
} Sha256Context;

void sha256_init(Sha256Context *context);
void sha256_update(Sha256Context *context, const uint8_t *data, size_t length);
void sha256_final(Sha256Context *context, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif /* CUP_SHA256_H */
