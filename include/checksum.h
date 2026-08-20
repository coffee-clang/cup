#ifndef CUP_CHECKSUM_H
#define CUP_CHECKSUM_H

/*
 * SHA-256 hashing and immutable SHA256SUMS snapshots used at runtime integrity boundaries.
 */

#include <stddef.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "system.h"

#define CHECKSUM_SHA256_HEX_LENGTH 64u

typedef struct {
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char name[MAX_METADATA_LINE_LEN];
} ChecksumEntry;

typedef struct {
    ChecksumEntry *entries;
    size_t count;
    SystemPathIdentity identity;
} ChecksumDocument;

/* Validate the canonical lowercase hexadecimal form of one SHA-256 digest. */
int checksum_digest_is_canonical(const char *value);

/*
 * Initialize before first load/free. A valid load attempt replaces prior state;
 * failure leaves an initialized empty snapshot.
 */
void checksum_document_init(ChecksumDocument *document);
void checksum_document_free(ChecksumDocument *document);
CupError checksum_document_load(ChecksumDocument *document, const char *path);

/*
 * Query and validate a snapshot produced by this lifecycle. Writable result
 * outputs are cleared on failure.
 */
CupError checksum_document_find_expected(const ChecksumDocument *document,
                                         const char *asset_name,
                                         char *hex,
                                         size_t size);
CupError checksum_document_validate_assets(const ChecksumDocument *document,
                                           const char *const *asset_names,
                                           size_t asset_count);
CupError checksum_document_verify_file(const ChecksumDocument *document,
                                       const char *asset_name,
                                       const char *asset_path,
                                       int *matches);

/*
 * Hash bytes, one open regular stream, or one regular path into lowercase
 * hexadecimal; short output storage is BUFFER_TOO_SMALL.
 */
CupError checksum_sha256_bytes(const unsigned char *data, size_t data_size, char *hex, size_t size);
CupError checksum_sha256_stream(FILE *file, char *hex, size_t size);
CupError checksum_sha256_file(const char *path, char *hex, size_t size);

/* Path-level helpers load exactly one immutable checksum snapshot per call. */
CupError checksum_verify_file(const char *checksum_path,
                              const char *asset_name,
                              const char *asset_path,
                              int *matches);
CupError checksum_validate_assets(const char *checksum_path,
                                  const char *const *asset_names,
                                  size_t asset_count);

#endif /* CUP_CHECKSUM_H */
