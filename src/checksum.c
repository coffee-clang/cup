#include "checksum.h"

#include "constants.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_size;
} Sha256Context;

static const uint32_t SHA256_CONSTANTS[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t load_be32(const unsigned char *data) {
    return ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        (uint32_t)data[3];
}

static void store_be32(unsigned char *data, uint32_t value) {
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static void sha256_transform(Sha256Context *context,
    const unsigned char block[64]) {
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0; i < 16; ++i) {
        words[i] = load_be32(block + i * 4);
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotate_right(words[i - 15], 7) ^
            rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = rotate_right(words[i - 2], 17) ^
            rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
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
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
            rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + sum1 + choose + SHA256_CONSTANTS[i] + words[i];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
            rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
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

static void sha256_init(Sha256Context *context) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    memcpy(context->state, initial, sizeof(initial));
    context->bit_count = 0;
    context->block_size = 0;
}

static void sha256_update(Sha256Context *context,
    const unsigned char *data, size_t size) {
    while (size > 0) {
        size_t available = sizeof(context->block) - context->block_size;
        size_t chunk = size < available ? size : available;

        memcpy(context->block + context->block_size, data, chunk);
        context->block_size += chunk;
        context->bit_count += (uint64_t)chunk * 8u;
        data += chunk;
        size -= chunk;

        if (context->block_size == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->block_size = 0;
        }
    }
}

static void sha256_finish(Sha256Context *context, unsigned char digest[32]) {
    uint64_t bit_count = context->bit_count;
    size_t i;

    context->block[context->block_size++] = 0x80u;
    if (context->block_size > 56) {
        memset(context->block + context->block_size, 0,
            sizeof(context->block) - context->block_size);
        sha256_transform(context, context->block);
        context->block_size = 0;
    }

    memset(context->block + context->block_size, 0, 56 - context->block_size);
    for (i = 0; i < 8; ++i) {
        context->block[63 - i] = (unsigned char)(bit_count >> (i * 8));
    }
    sha256_transform(context, context->block);

    for (i = 0; i < 8; ++i) {
        store_be32(digest + i * 4, context->state[i]);
    }
}

static int is_hex_digest(const char *value) {
    size_t i;

    if (value == NULL || strlen(value) != SHA256_HEX_LENGTH) {
        return 0;
    }
    for (i = 0; i < SHA256_HEX_LENGTH; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static CupError parse_checksum_line(char *line, char **digest, char **name) {
    char *cursor;

    if (line == NULL || digest == NULL || name == NULL ||
        strlen(line) < SHA256_HEX_LENGTH + 2) {
        return CUP_ERR_VALIDATION;
    }

    cursor = line + SHA256_HEX_LENGTH;
    if (*cursor != ' ' && *cursor != '\t') {
        return CUP_ERR_VALIDATION;
    }
    *cursor++ = '\0';
    if (!is_hex_digest(line)) {
        return CUP_ERR_VALIDATION;
    }

    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor == '*') {
        cursor++;
    }
    if (!path_is_safe_segment(cursor)) {
        return CUP_ERR_VALIDATION;
    }

    *digest = line;
    *name = cursor;
    return CUP_OK;
}

CupError checksum_sha256_file(const char *path, char *hex, size_t size) {
    Sha256Context context;
    unsigned char digest[32];
    unsigned char buffer[8192];
    FILE *file;
    size_t read_count;
    size_t i;

    if (path == NULL || hex == NULL || size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_INVALID_INPUT;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    sha256_init(&context);
    while ((read_count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sha256_update(&context, buffer, read_count);
    }
    {
        int read_failed = ferror(file);
        int close_failed = fclose(file) != 0;

        if (read_failed || close_failed) {
            return CUP_ERR_FILESYSTEM;
        }
    }

    sha256_finish(&context, digest);
    for (i = 0; i < sizeof(digest); ++i) {
        static const char digits[] = "0123456789abcdef";
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0fu];
    }
    hex[SHA256_HEX_LENGTH] = '\0';
    return CUP_OK;
}

CupError checksum_find_expected(const char *checksum_path,
    const char *asset_name, char *hex, size_t size) {
    FILE *file;
    CupError err;
    char line[MAX_INFO_LINE_LEN];
    size_t line_number = 0;
    int found = 0;

    if (checksum_path == NULL || !path_is_safe_segment(asset_name) ||
        hex == NULL || size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_INVALID_INPUT;
    }

    file = fopen(checksum_path, "r");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    while (1) {
        char *digest;
        char *name;
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
        }
        if (!has_line) {
            break;
        }
        if (parse_checksum_line(line, &digest, &name) != CUP_OK) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
        if (strcmp(name, asset_name) != 0) {
            continue;
        }
        if (found) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
        if (text_format(hex, size, "%s", digest) != CUP_OK) {
            fclose(file);
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        {
            size_t i;
            for (i = 0; hex[i] != '\0'; ++i) {
                if (hex[i] >= 'A' && hex[i] <= 'F') {
                    hex[i] = (char)(hex[i] - 'A' + 'a');
                }
            }
        }
        found = 1;
    }

    if (fclose(file) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return found ? CUP_OK : CUP_ERR_VALIDATION;
}

CupError checksum_verify_file(const char *checksum_path,
    const char *asset_name, const char *asset_path, int *matches) {
    char expected[SHA256_HEX_LENGTH + 1];
    char actual[SHA256_HEX_LENGTH + 1];
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    err = checksum_find_expected(checksum_path, asset_name,
        expected, sizeof(expected));
    if (err != CUP_OK) {
        return err;
    }
    err = checksum_sha256_file(asset_path, actual, sizeof(actual));
    if (err != CUP_OK) {
        return err;
    }

    *matches = strcmp(expected, actual) == 0;
    return CUP_OK;
}

CupError checksum_validate_file(const char *checksum_path,
    size_t *entry_count) {
    FILE *file;
    CupError err;
    char line[MAX_INFO_LINE_LEN];
    size_t line_number = 0;
    size_t count = 0;

    if (checksum_path == NULL || entry_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *entry_count = 0;

    file = fopen(checksum_path, "r");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    while (1) {
        char *digest;
        char *name;
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
        }
        if (!has_line) {
            break;
        }
        if (parse_checksum_line(line, &digest, &name) != CUP_OK) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
        (void)digest;
        (void)name;
        count++;
    }

    if (fclose(file) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    if (count == 0) {
        return CUP_ERR_VALIDATION;
    }

    *entry_count = count;
    return CUP_OK;
}

CupError checksum_validate_assets(const char *checksum_path,
    const char *const *asset_names, size_t asset_count) {
    char digest[SHA256_HEX_LENGTH + 1];
    size_t entry_count;
    size_t i;
    CupError err;

    if (checksum_path == NULL || asset_names == NULL || asset_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checksum_validate_file(checksum_path, &entry_count);
    if (err != CUP_OK) {
        return err;
    }
    if (entry_count != asset_count) {
        return CUP_ERR_VALIDATION;
    }

    for (i = 0; i < asset_count; ++i) {
        err = checksum_find_expected(checksum_path, asset_names[i],
            digest, sizeof(digest));
        if (err != CUP_OK) {
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
        }
    }
    return CUP_OK;
}
