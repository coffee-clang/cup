/*
 * Implements the SHA-256 digest primitive used for public release-file integrity checks.
 * Checksum-file policy is intentionally kept in checksum.c.
 */

/*
 * SHA-256 implementation adapted for cup from Brad Conte's public-domain
 * crypto-algorithms/sha256.c:
 * https://github.com/B-Con/crypto-algorithms/blob/master/sha256.c
 *
 * The upstream author releases the implementation into the public domain,
 * without warranty. cup changes the names and fixed-width types and keeps the
 * compression routine private; the algorithm is unchanged.
 */

#include "sha256.h"

#include <string.h>

#define ROTATE_RIGHT(value, bits) (((value) >> (bits)) | ((value) << (32u - (bits))))
#define CHOICE(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJORITY(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTATE_RIGHT((x), 2u) ^ ROTATE_RIGHT((x), 13u) ^ ROTATE_RIGHT((x), 22u))
#define EP1(x) (ROTATE_RIGHT((x), 6u) ^ ROTATE_RIGHT((x), 11u) ^ ROTATE_RIGHT((x), 25u))
#define SIG0(x) (ROTATE_RIGHT((x), 7u) ^ ROTATE_RIGHT((x), 18u) ^ ((x) >> 3u))
#define SIG1(x) (ROTATE_RIGHT((x), 17u) ^ ROTATE_RIGHT((x), 19u) ^ ((x) >> 10u))

static const uint32_t round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static void transform(Sha256Context *context, const uint8_t data[64]) {
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t first, second;
    size_t i;

    for (i = 0; i < 16; ++i) {
        size_t offset = i * 4;
        words[i] = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                   ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];
    }
    for (; i < 64; ++i) {
        words[i] = SIG1(words[i - 2]) + words[i - 7] + SIG0(words[i - 15]) + words[i - 16];
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (i = 0; i < 64; ++i) {
        first = h + EP1(e) + CHOICE(e, f, g) + round_constants[i] + words[i];
        second = EP0(a) + MAJORITY(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void sha256_init(Sha256Context *context) {
    context->data_length = 0;
    context->bit_length = 0;
    context->state[0] = 0x6a09e667u;
    context->state[1] = 0xbb67ae85u;
    context->state[2] = 0x3c6ef372u;
    context->state[3] = 0xa54ff53au;
    context->state[4] = 0x510e527fu;
    context->state[5] = 0x9b05688cu;
    context->state[6] = 0x1f83d9abu;
    context->state[7] = 0x5be0cd19u;
}

void sha256_update(Sha256Context *context, const uint8_t *data, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        context->data[context->data_length++] = data[i];
        if (context->data_length == sizeof(context->data)) {
            transform(context, context->data);
            context->bit_length += 512u;
            context->data_length = 0;
        }
    }
}

void sha256_final(Sha256Context *context, uint8_t digest[SHA256_DIGEST_SIZE]) {
    size_t i = context->data_length;

    context->data[i++] = 0x80u;
    if (i > 56) {
        while (i < 64) {
            context->data[i++] = 0;
        }
        transform(context, context->data);
        memset(context->data, 0, 56);
    } else {
        while (i < 56) {
            context->data[i++] = 0;
        }
    }

    context->bit_length += (uint64_t)context->data_length * 8u;
    context->data[63] = (uint8_t)context->bit_length;
    context->data[62] = (uint8_t)(context->bit_length >> 8);
    context->data[61] = (uint8_t)(context->bit_length >> 16);
    context->data[60] = (uint8_t)(context->bit_length >> 24);
    context->data[59] = (uint8_t)(context->bit_length >> 32);
    context->data[58] = (uint8_t)(context->bit_length >> 40);
    context->data[57] = (uint8_t)(context->bit_length >> 48);
    context->data[56] = (uint8_t)(context->bit_length >> 56);
    transform(context, context->data);

    for (i = 0; i < 4; ++i) {
        digest[i] = (uint8_t)(context->state[0] >> (24u - i * 8u));
        digest[i + 4] = (uint8_t)(context->state[1] >> (24u - i * 8u));
        digest[i + 8] = (uint8_t)(context->state[2] >> (24u - i * 8u));
        digest[i + 12] = (uint8_t)(context->state[3] >> (24u - i * 8u));
        digest[i + 16] = (uint8_t)(context->state[4] >> (24u - i * 8u));
        digest[i + 20] = (uint8_t)(context->state[5] >> (24u - i * 8u));
        digest[i + 24] = (uint8_t)(context->state[6] >> (24u - i * 8u));
        digest[i + 28] = (uint8_t)(context->state[7] >> (24u - i * 8u));
    }
}
