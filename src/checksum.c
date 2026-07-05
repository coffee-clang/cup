#include "checksum.h"

#include "constants.h"
#include "path.h"
#include "sha256.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

static CupError digest_file(const char *path, unsigned char *digest) {
    Sha256Context context;
    unsigned char buffer[8192];
    FILE *file;
    CupError err = CUP_OK;

    file = fopen(path, "rb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    sha256_init(&context);
    while (1) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);

        if (count > 0) {
            sha256_update(&context, buffer, count);
        }
        if (count < sizeof(buffer)) {
            if (ferror(file)) {
                err = CUP_ERR_FILESYSTEM;
            }
            break;
        }
    }

    if (err == CUP_OK) {
        sha256_final(&context, digest);
    }
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    return err;
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
    unsigned char digest[SHA256_DIGEST_SIZE];
    static const char digits[] = "0123456789abcdef";
    CupError err;
    size_t i;

    if (path == NULL || hex == NULL || size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = digest_file(path, digest);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < SHA256_DIGEST_SIZE; ++i) {
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
        if (text_copy(hex, size, digest) != CUP_OK) {
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
