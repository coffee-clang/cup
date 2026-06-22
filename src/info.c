#include "info.h"

#include "path.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// METADATA LIFECYCLE
void info_init(PackageInfo *info) {
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
}

void info_free(PackageInfo *info) {
    if (info == NULL) {
        return;
    }

    free(info->fields);
    info_init(info);
}

// METADATA VALIDATION
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
    static const char *const groups[] = {
        "entry.", "features.", "contents.", "config."
    };
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

// METADATA STORAGE
static int key_has_prefix(const char *key, const char *prefix) {
    size_t length;

    if (text_is_empty(key) || text_is_empty(prefix)) {
        return 0;
    }

    length = strlen(prefix);
    return strncmp(key, prefix, length) == 0;
}

static int info_has_key(const PackageInfo *info, const char *key) {
    size_t i;

    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError add_field(PackageInfo *info, const char *key, const char *value) {
    PackageInfoField *fields;
    size_t capacity;

    if (info_has_key(info, key)) {
        return CUP_ERR_VALIDATION;
    }

    if (info->count == info->capacity) {
        capacity = info->capacity == 0 ? 16 : info->capacity * 2;
        fields = realloc(info->fields, capacity * sizeof(*fields));
        if (fields == NULL) {
            return CUP_ERR_VALIDATION;
        }

        info->fields = fields;
        info->capacity = capacity;
    }

    if (text_copy(info->fields[info->count].key,
            sizeof(info->fields[info->count].key), key) != CUP_OK ||
        text_copy(info->fields[info->count].value,
            sizeof(info->fields[info->count].value), value) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    info->count++;
    return CUP_OK;
}

// METADATA LOADING
CupError info_load(PackageInfo *info, const char *path) {
    FILE *file;
    CupError err;
    char line[MAX_INFO_LINE_LEN];
    size_t line_number = 0;

    if (info == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    info_free(info);

    file = fopen(path, "r");
    if (file == NULL) {
        return CUP_ERR_VALIDATION;
    }

    while (1) {
        char key[MAX_INFO_KEY_LEN];
        char value[MAX_INFO_VALUE_LEN];
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            info_free(info);
            return CUP_ERR_VALIDATION;
        }

        if (!has_line) {
            break;
        }

        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK ||
            !key_is_safe(key) || !value_is_safe(value) ||
            add_field(info, key, value) != CUP_OK) {
            fprintf(stderr, "Error: invalid package metadata line %zu.\n", line_number);
            fclose(file);
            info_free(info);
            return CUP_ERR_VALIDATION;
        }
    }

    if (fclose(file) != 0) {
        info_free(info);
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

// METADATA QUERIES
const char *info_get(const PackageInfo *info, const char *key) {
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

const PackageInfoField *info_next(const PackageInfo *info, const char *prefix, size_t *cursor) {
    if (info == NULL || text_is_empty(prefix) || cursor == NULL) {
        return NULL;
    }

    while (*cursor < info->count) {
        const PackageInfoField *field = &info->fields[*cursor];

        (*cursor)++;
        if (key_has_prefix(field->key, prefix)) {
            return field;
        }
    }

    return NULL;
}
