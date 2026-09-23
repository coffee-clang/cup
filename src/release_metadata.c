/* Parses one canonical release.txt format-2 manifest without accepting partial input. */

#include "release_metadata.h"

#include "filesystem.h"
#include "text.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CupError release_version_parse(const char *text, ReleaseVersion *version) {
    const char *cursor;
    char *end;
    unsigned long parts[3];
    size_t i;

    if (version != NULL) {
        memset(version, 0, sizeof(*version));
    }
    if (text_is_empty(text)) {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = text;
    for (i = 0; i < 3; ++i) {
        if (*cursor < '0' || *cursor > '9' ||
            (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')) {
            return CUP_ERR_VALIDATION;
        }
        errno = 0;
        parts[i] = strtoul(cursor, &end, 10);
        if (end == cursor || errno != 0 || parts[i] > 999999u) {
            return CUP_ERR_VALIDATION;
        }
        if (i < 2) {
            if (*end != '.') {
                return CUP_ERR_VALIDATION;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return CUP_ERR_VALIDATION;
        }
    }

    if (version != NULL) {
        version->major = (unsigned)parts[0];
        version->minor = (unsigned)parts[1];
        version->patch = (unsigned)parts[2];
    }
    return CUP_OK;
}

static int digest_is_canonical(const char *value) {
    size_t i;
    if (value == NULL || strlen(value) != 64u) return 0;
    for (i = 0; i < 64u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    }
    return 1;
}

static int commit_is_valid(const char *value) {
    size_t i;
    if (text_is_empty(value) || strlen(value) != 40u) {
        return 0;
    }
    for (i = 0; i < 40u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static CupError parse_uint(const char *value, unsigned *out) {
    char *end;
    unsigned long parsed;

    if (text_is_empty(value) || out == NULL ||
        (*value == '0' && value[1] != '\0')) {
        return CUP_ERR_VALIDATION;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        return CUP_ERR_VALIDATION;
    }
    *out = (unsigned)parsed;
    return CUP_OK;
}

static CupError parse_size(const char *value, size_t *out) {
    unsigned parsed;
    CupError err = parse_uint(value, &parsed);
    if (err != CUP_OK) {
        return err;
    }
    *out = (size_t)parsed;
    return CUP_OK;
}

static int asset_name_is_valid(const char *name) {
    size_t i;
    if (text_is_empty(name) || strlen(name) >= MAX_PATH_SEGMENT_LEN ||
        strcmp(name, CUP_RELEASE_METADATA_FILENAME) == 0 ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; name[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x21u || c > 0x7eu || c == '/' || c == '\\' || c == '=') {
            return 0;
        }
    }
    return 1;
}

void release_metadata_init(ReleaseMetadata *metadata) {
    if (metadata != NULL) {
        memset(metadata, 0, sizeof(*metadata));
    }
}

void release_metadata_free(ReleaseMetadata *metadata) {
    if (metadata != NULL) {
        free(metadata->assets);
        memset(metadata, 0, sizeof(*metadata));
    }
}

static CupError read_expected_line(TextDocumentReader *reader,
                                   char *line,
                                   size_t size,
                                   const char *key,
                                   const char **value) {
    size_t key_len = strlen(key);
    int has_line;
    CupError err = text_document_read_raw_line(reader, line, size, &has_line);
    if (err != CUP_OK || !has_line || strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
        return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    }
    *value = line + key_len + 1u;
    return text_is_empty(*value) ? CUP_ERR_VALIDATION : CUP_OK;
}

CupError release_metadata_load(const char *path, ReleaseMetadata *metadata) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    ReleaseMetadata candidate;
    CupError err;
    char line[MAX_METADATA_LINE_LEN];
    const char *value;
    size_t i;
    int missing;

    if (text_is_empty(path) || metadata == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    release_metadata_init(&candidate);
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, CUP_RELEASE_MANIFEST_MAX_BYTES, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        filesystem_snapshot_release(&snapshot);
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "format", &value);
    if (err == CUP_OK && strcmp(value, "2") != 0) err = CUP_ERR_VALIDATION;
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "version", &value);
    if (err == CUP_OK) err = text_copy(candidate.version, sizeof(candidate.version), value);
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "commit", &value);
    if (err == CUP_OK) err = text_copy(candidate.commit, sizeof(candidate.commit), value);
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "root_layout", &value);
    if (err == CUP_OK) err = parse_uint(value, &candidate.root_layout);
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "catalog_format", &value);
    if (err == CUP_OK) err = parse_uint(value, &candidate.catalog_format);
    if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), "asset_count", &value);
    if (err == CUP_OK) err = parse_size(value, &candidate.asset_count);
    if (err == CUP_OK && candidate.asset_count > CUP_RELEASE_MANIFEST_MAX_BYTES / 80u) {
        err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK && candidate.asset_count != 0u) {
        candidate.assets = calloc(candidate.asset_count, sizeof(*candidate.assets));
        if (candidate.assets == NULL) err = CUP_ERR_TEMPORARY;
    }
    for (i = 0; err == CUP_OK && i < candidate.asset_count; ++i) {
        char key[64];
        int written = snprintf(key, sizeof(key), "asset.%zu.name", i);
        if (written < 0 || (size_t)written >= sizeof(key)) { err = CUP_ERR_VALIDATION; break; }
        err = read_expected_line(&reader, line, sizeof(line), key, &value);
        if (err == CUP_OK && (!asset_name_is_valid(value) ||
                              text_copy(candidate.assets[i].name, sizeof(candidate.assets[i].name), value) != CUP_OK)) {
            err = CUP_ERR_VALIDATION;
        }
        written = snprintf(key, sizeof(key), "asset.%zu.sha256", i);
        if (err == CUP_OK && (written < 0 || (size_t)written >= sizeof(key))) err = CUP_ERR_VALIDATION;
        if (err == CUP_OK) err = read_expected_line(&reader, line, sizeof(line), key, &value);
        if (err == CUP_OK && (!digest_is_canonical(value) ||
                              text_copy(candidate.assets[i].sha256, sizeof(candidate.assets[i].sha256), value) != CUP_OK)) {
            err = CUP_ERR_VALIDATION;
        }
        if (err == CUP_OK && i > 0 && strcmp(candidate.assets[i - 1u].name, candidate.assets[i].name) >= 0) {
            err = CUP_ERR_VALIDATION;
        }
    }
    if (err == CUP_OK) {
        int has_line;
        err = text_document_read_raw_line(&reader, line, sizeof(line), &has_line);
        if (err == CUP_OK && has_line) err = CUP_ERR_VALIDATION;
    }
    filesystem_snapshot_release(&snapshot);
    if (err == CUP_OK && release_version_parse(candidate.version, NULL) != CUP_OK) err = CUP_ERR_VALIDATION;
    if (err == CUP_OK && !commit_is_valid(candidate.commit)) err = CUP_ERR_VALIDATION;
    if (err != CUP_OK) {
        release_metadata_free(&candidate);
        return err == CUP_ERR_FILESYSTEM || err == CUP_ERR_BUFFER_TOO_SMALL || err == CUP_ERR_TEMPORARY
                   ? err : CUP_ERR_VALIDATION;
    }
    *metadata = candidate;
    return CUP_OK;
}

const ReleaseAsset *release_metadata_find_asset(const ReleaseMetadata *metadata, const char *name) {
    size_t low = 0, high;
    if (metadata == NULL || text_is_empty(name)) return NULL;
    high = metadata->asset_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(metadata->assets[mid].name, name);
        if (cmp == 0) return &metadata->assets[mid];
        if (cmp < 0) low = mid + 1u; else high = mid;
    }
    return NULL;
}
