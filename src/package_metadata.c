/*
 * Loads immutable package info.txt key/value metadata and provides ordered field lookup without
 * interpreting package policy.
 */

#include "package_metadata.h"

#include "path.h"
#include "text.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Metadata lifecycle. */
void package_metadata_init(PackageMetadata *info) {
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
}

void package_metadata_free(PackageMetadata *info) {
    if (info == NULL) {
        return;
    }

    free(info->fields);
    package_metadata_init(info);
}

/* Metadata validation. */
static int value_is_safe(const char *value) {
    const unsigned char *cursor;

    if (text_is_empty(value)) {
        return 0;
    }

    for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        if (*cursor < 32 || *cursor == 127) {
            return 0;
        }
    }

    return 1;
}

static int key_is_safe(const char *key) {
    static const char *const groups[] = {"entry.", "features.", "contents.", "config."};
    size_t i;

    if (!path_is_safe_identifier(key)) {
        return 0;
    }

    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        size_t length = strlen(groups[i]);

        if (strncmp(key, groups[i], length) == 0) {
            return path_is_safe_identifier(key + length);
        }
    }

    return 1;
}

/* Metadata storage. */
static int key_has_prefix(const char *key, const char *prefix) {
    size_t length;

    if (text_is_empty(key) || text_is_empty(prefix)) {
        return 0;
    }

    length = strlen(prefix);
    return strncmp(key, prefix, length) == 0;
}

static int package_metadata_has_key(const PackageMetadata *info, const char *key) {
    size_t i;

    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError add_field(PackageMetadata *info, const char *key, const char *value) {
    PackageMetadataField *fields;
    size_t capacity;

    if (info == NULL || text_is_empty(key) || text_is_empty(value)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (package_metadata_has_key(info, key)) {
        return CUP_ERR_VALIDATION;
    }

    if (info->count == info->capacity) {
        if (info->capacity > SIZE_MAX / 2) {
            return CUP_ERR_TEMPORARY;
        }
        capacity = info->capacity == 0 ? 16 : info->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*fields)) {
            return CUP_ERR_TEMPORARY;
        }
        fields = realloc(info->fields, capacity * sizeof(*fields));
        if (fields == NULL) {
            return CUP_ERR_TEMPORARY;
        }

        info->fields = fields;
        info->capacity = capacity;
    }

    if (text_copy(info->fields[info->count].key, sizeof(info->fields[info->count].key), key) !=
            CUP_OK ||
        text_copy(info->fields[info->count].value,
                  sizeof(info->fields[info->count].value),
                  value) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    info->count++;
    return CUP_OK;
}

/* Metadata loading. */
CupError package_metadata_load(PackageMetadata *info, const char *path) {
    FILE *file;
    CupError err;
    char line[MAX_METADATA_LINE_LEN];
    size_t line_number = 0;

    if (info == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package_metadata_free(info);

    file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? CUP_ERR_VALIDATION : CUP_ERR_FILESYSTEM;
    }

    while (1) {
        char key[MAX_METADATA_KEY_LEN];
        char value[MAX_METADATA_VALUE_LEN];
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            package_metadata_free(info);
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
        }

        if (!has_line) {
            break;
        }

        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK ||
            !key_is_safe(key) || !value_is_safe(value)) {
            fprintf(stderr, "Error: invalid package metadata line %zu.\n", line_number);
            fclose(file);
            package_metadata_free(info);
            return CUP_ERR_VALIDATION;
        }

        err = add_field(info, key, value);
        if (err != CUP_OK) {
            if (err == CUP_ERR_VALIDATION) {
                fprintf(
                    stderr, "Error: duplicate package metadata key on line %zu.\n", line_number);
            }
            fclose(file);
            package_metadata_free(info);
            return err;
        }
    }

    if (fclose(file) != 0) {
        package_metadata_free(info);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

/* Metadata queries. */
const char *package_metadata_get(const PackageMetadata *info, const char *key) {
    size_t i;

    if (info == NULL || text_is_empty(key)) {
        return NULL;
    }

    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return info->fields[i].value;
        }
    }

    return NULL;
}

const PackageMetadataField *package_metadata_next(const PackageMetadata *metadata,
                                                  const char *prefix,
                                                  size_t *cursor) {
    if (metadata == NULL || text_is_empty(prefix) || cursor == NULL) {
        return NULL;
    }

    while (*cursor < metadata->count) {
        const PackageMetadataField *field = &metadata->fields[*cursor];

        (*cursor)++;
        if (key_has_prefix(field->key, prefix)) {
            return field;
        }
    }

    return NULL;
}

int package_metadata_next_command(const PackageMetadata *metadata,
                                  PackageCommand *command,
                                  size_t *cursor) {
    const PackageMetadataField *field;
    static const char prefix[] = "entry.";

    if (metadata == NULL || command == NULL || cursor == NULL) {
        return 0;
    }

    field = package_metadata_next(metadata, prefix, cursor);
    if (field == NULL) {
        return 0;
    }

    memset(command, 0, sizeof(*command));
    if (text_copy(command->name, sizeof(command->name), field->key + sizeof(prefix) - 1) !=
            CUP_OK ||
        text_copy(command->path, sizeof(command->path), field->value) != CUP_OK) {
        return 0;
    }

    return 1;
}
