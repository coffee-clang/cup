#ifndef CUP_CHECKSUM_H
#define CUP_CHECKSUM_H

#include <stddef.h>

#include "error.h"

#define SHA256_HEX_LENGTH 64

CupError checksum_sha256_file(const char *path, char *hex, size_t size);
CupError checksum_find_expected(const char *checksum_path,
    const char *asset_name, char *hex, size_t size);
CupError checksum_verify_file(const char *checksum_path,
    const char *asset_name, const char *asset_path, int *matches);
CupError checksum_validate_file(const char *checksum_path,
    size_t *entry_count);
CupError checksum_validate_assets(const char *checksum_path,
    const char *const *asset_names, size_t asset_count);

#endif /* CUP_CHECKSUM_H */
