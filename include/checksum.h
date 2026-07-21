#ifndef CUP_CHECKSUM_H
#define CUP_CHECKSUM_H

/*
 * Module contract: SHA-256 file hashing and strict SHA256SUMS selection
 * helpers used at runtime integrity boundaries.
 */

#include <stddef.h>

#include "error.h"

#define SHA256_HEX_LENGTH 64

/* Hash one regular file and write a lowercase hexadecimal digest. */
CupError checksum_sha256_file(const char *path, char *hex, size_t size);

/* Find exactly one checksum entry whose filename matches asset_name. */
CupError checksum_find_expected(const char *checksum_path,
                                const char *asset_name,
                                char *hex,
                                size_t size);

/* Compare one asset with its unique checksum entry. */
CupError checksum_verify_file(const char *checksum_path,
                              const char *asset_name,
                              const char *asset_path,
                              int *matches);

/* Validate the complete checksum file and optionally return its entry count. */
CupError checksum_validate_file(const char *checksum_path, size_t *entry_count);

/* Require the checksum file to contain exactly the expected asset set. */
CupError checksum_validate_assets(const char *checksum_path,
                                  const char *const *asset_names,
                                  size_t asset_count);

#endif /* CUP_CHECKSUM_H */
