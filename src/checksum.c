/*
 * Owns immutable SHA256SUMS snapshots and compares selected assets with the adapted
 * third-party SHA-256
 * implementation. One parsed document is reused for exact-set validation and later lookups.
 */

#include "checksum.h"

#include "third_party/sha256.h"

#include "filesystem.h"
#include "interrupt.h"
#include "path.h"
#include "text.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static CupError parse_checksum_line(char *line, char **digest, char **name) {
    char *cursor;

    if (line == NULL || digest == NULL || name == NULL || strlen(line) < SHA256_HEX_LENGTH + 2) {
        return CUP_ERR_VALIDATION;
    }

    cursor = line + SHA256_HEX_LENGTH;
    if (cursor[0] != ' ' || (cursor[1] != ' ' && cursor[1] != '*')) {
        return CUP_ERR_VALIDATION;
    }
    *cursor = '\0';
    cursor += 2;
    if (!checksum_digest_is_canonical(line) || !path_is_safe_segment(cursor)) {
        return CUP_ERR_VALIDATION;
    }

    *digest = line;
    *name = cursor;
    return CUP_OK;
}

void checksum_document_init(ChecksumDocument *document) {
    if (document != NULL) {
        memset(document, 0, sizeof(*document));
    }
}

void checksum_document_free(ChecksumDocument *document) {
    if (document != NULL) {
        free(document->entries);
        checksum_document_init(document);
    }
}

static const ChecksumEntry *checksum_document_find_entry(const ChecksumDocument *document,
                                                         const char *name) {
    size_t i;

    if (document == NULL || text_is_empty(name)) {
        return NULL;
    }
    for (i = 0; i < document->count; ++i) {
        if (strcmp(document->entries[i].name, name) == 0) {
            return &document->entries[i];
        }
    }
    return NULL;
}

CupError checksum_document_load(ChecksumDocument *document, const char *path) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupError err;
    char line[MAX_METADATA_LINE_LEN];
    size_t capacity = 0;
    int missing;

    if (document == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    checksum_document_free(document);
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, MAX_PERSISTENT_METADATA_BYTES, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    if (err != CUP_OK) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_VALIDATION;
    }

    while (1) {
        char *digest;
        char *name;
        ChecksumEntry *grown;
        int has_line;

        err = text_document_read_raw_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK) {
            checksum_document_free(document);
            filesystem_snapshot_release(&snapshot);
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
        }
        if (!has_line) {
            break;
        }
        if (parse_checksum_line(line, &digest, &name) != CUP_OK ||
            checksum_document_find_entry(document, name) != NULL) {
            checksum_document_free(document);
            filesystem_snapshot_release(&snapshot);
            return CUP_ERR_VALIDATION;
        }
        if (document->count == capacity) {
            size_t next = capacity == 0 ? 8u : capacity * 2u;
            if (next > MAX_PERSISTENT_METADATA_BYTES / sizeof(*document->entries)) {
                checksum_document_free(document);
                filesystem_snapshot_release(&snapshot);
                return CUP_ERR_BUFFER_TOO_SMALL;
            }
            grown = realloc(document->entries, next * sizeof(*document->entries));
            if (grown == NULL) {
                checksum_document_free(document);
                filesystem_snapshot_release(&snapshot);
                return CUP_ERR_TEMPORARY;
            }
            document->entries = grown;
            capacity = next;
        }
        if (text_copy(document->entries[document->count].digest,
                      sizeof(document->entries[document->count].digest),
                      digest) != CUP_OK ||
            text_copy(document->entries[document->count].name,
                      sizeof(document->entries[document->count].name),
                      name) != CUP_OK) {
            checksum_document_free(document);
            filesystem_snapshot_release(&snapshot);
            return CUP_ERR_VALIDATION;
        }
        document->count++;
    }

    if (reader.line_number != document->count || document->count == 0) {
        checksum_document_free(document);
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_VALIDATION;
    }
    document->identity = snapshot.identity;
    filesystem_snapshot_release(&snapshot);
    return CUP_OK;
}

CupError checksum_document_find_expected(const ChecksumDocument *document,
                                         const char *asset_name,
                                         char *hex,
                                         size_t size) {
    const ChecksumEntry *entry;

    if (hex != NULL && size > 0) {
        hex[0] = '\0';
    }
    if (document == NULL || !path_is_safe_segment(asset_name) || hex == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (size < SHA256_HEX_LENGTH + 1) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    entry = checksum_document_find_entry(document, asset_name);
    return entry == NULL ? CUP_ERR_VALIDATION : text_copy(hex, size, entry->digest);
}

CupError checksum_document_validate_assets(const ChecksumDocument *document,
                                           const char *const *asset_names,
                                           size_t asset_count) {
    size_t i;

    if (document == NULL || asset_names == NULL || asset_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; i < asset_count; ++i) {
        size_t j;

        if (!path_is_safe_segment(asset_names[i])) {
            return CUP_ERR_INVALID_INPUT;
        }
        for (j = 0; j < i; ++j) {
            if (strcmp(asset_names[j], asset_names[i]) == 0) {
                return CUP_ERR_INVALID_INPUT;
            }
        }
    }
    if (document->count != asset_count) {
        return CUP_ERR_VALIDATION;
    }
    for (i = 0; i < asset_count; ++i) {
        if (checksum_document_find_entry(document, asset_names[i]) == NULL) {
            return CUP_ERR_VALIDATION;
        }
    }
    return CUP_OK;
}

CupError checksum_document_verify_file(const ChecksumDocument *document,
                                       const char *asset_name,
                                       const char *asset_path,
                                       int *matches) {
    char expected[SHA256_HEX_LENGTH + 1];
    char actual[SHA256_HEX_LENGTH + 1];
    CupError err;

    if (matches != NULL) {
        *matches = 0;
    }
    if (matches == NULL || document == NULL || !path_is_safe_segment(asset_name) ||
        text_is_empty(asset_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = checksum_document_find_expected(document, asset_name, expected, sizeof(expected));
    if (err == CUP_OK) {
        err = checksum_sha256_file(asset_path, actual, sizeof(actual));
    }
    if (err == CUP_OK) {
        *matches = strcmp(expected, actual) == 0;
    }
    return err;
}

CupError checksum_verify_file(const char *checksum_path,
                              const char *asset_name,
                              const char *asset_path,
                              int *matches) {
    ChecksumDocument document;
    CupError err;

    if (matches != NULL) {
        *matches = 0;
    }
    if (matches == NULL || text_is_empty(checksum_path) || !path_is_safe_segment(asset_name) ||
        text_is_empty(asset_path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    checksum_document_init(&document);
    err = checksum_document_load(&document, checksum_path);
    if (err == CUP_OK) {
        err = checksum_document_verify_file(&document, asset_name, asset_path, matches);
    }
    checksum_document_free(&document);
    return err;
}

CupError checksum_validate_assets(const char *checksum_path,
                                  const char *const *asset_names,
                                  size_t asset_count) {
    ChecksumDocument document;
    CupError err;
    size_t i;

    if (text_is_empty(checksum_path) || asset_names == NULL || asset_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; i < asset_count; ++i) {
        size_t j;
        if (!path_is_safe_segment(asset_names[i])) {
            return CUP_ERR_INVALID_INPUT;
        }
        for (j = 0; j < i; ++j) {
            if (strcmp(asset_names[j], asset_names[i]) == 0) {
                return CUP_ERR_INVALID_INPUT;
            }
        }
    }
    checksum_document_init(&document);
    err = checksum_document_load(&document, checksum_path);
    if (err == CUP_OK) {
        err = checksum_document_validate_assets(&document, asset_names, asset_count);
    }
    checksum_document_free(&document);
    return err;
}
