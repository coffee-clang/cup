/* Parses one canonical release.txt snapshot without accepting partial or reordered input. */

#include "release_metadata.h"

#include "filesystem.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>

#define RELEASE_METADATA_MAX_BYTES                                                   \
    ((sizeof("format=1\n") - 1u) + (sizeof("version=") - 1u) +                    \
     (CUP_RELEASE_VERSION_MAX - 1u) + 1u + (sizeof("commit=") - 1u) +             \
     (CUP_RELEASE_COMMIT_MAX - 1u) + 1u)

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
        parts[i] = strtoul(cursor, &end, 10);
        if (end == cursor || parts[i] > 999999u) {
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

static int commit_is_valid(const char *value) {
    size_t i;
    size_t length;

    if (text_is_empty(value)) {
        return 0;
    }
    length = strlen(value);
    if (length != 40) {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

CupError release_metadata_load(const char *path, ReleaseMetadata *metadata) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupError err;
    char line[256];
    size_t index = 0;
    int missing;

    if (text_is_empty(path) || metadata == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(metadata, 0, sizeof(*metadata));
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, RELEASE_METADATA_MAX_BYTES, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    while (err == CUP_OK) {
        const char *value;
        int has_line;

        err = text_document_read_raw_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK || !has_line) {
            break;
        }
        if (index >= 3u) {
            err = CUP_ERR_VALIDATION;
            break;
        }
        if (index == 0) {
            if (strcmp(line, "format=1") != 0) {
                err = CUP_ERR_VALIDATION;
            }
        } else if (index == 1) {
            if (strncmp(line, "version=", sizeof("version=") - 1u) != 0) {
                err = CUP_ERR_VALIDATION;
            } else {
                value = line + sizeof("version=") - 1u;
                if (text_is_empty(value) ||
                    text_copy(metadata->version, sizeof(metadata->version), value) != CUP_OK) {
                    err = CUP_ERR_VALIDATION;
                }
            }
        } else {
            if (strncmp(line, "commit=", sizeof("commit=") - 1u) != 0) {
                err = CUP_ERR_VALIDATION;
            } else {
                value = line + sizeof("commit=") - 1u;
                if (text_is_empty(value) ||
                    text_copy(metadata->commit, sizeof(metadata->commit), value) != CUP_OK) {
                    err = CUP_ERR_VALIDATION;
                }
            }
        }
        index++;
    }
    filesystem_snapshot_release(&snapshot);
    if (err != CUP_OK || index != 3u ||
        release_version_parse(metadata->version, NULL) != CUP_OK ||
        !commit_is_valid(metadata->commit)) {
        memset(metadata, 0, sizeof(*metadata));
        return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}
