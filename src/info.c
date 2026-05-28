#include "info.h"

#include "util.h"

#include <stdio.h>
#include <string.h>

int info_key_has_prefix(const char *key, const char *prefix) {
    size_t prefix_len;

    if (is_empty_string(key) || is_empty_string(prefix)) {
        return 0;
    }

    prefix_len = strlen(prefix);

    if (strncmp(key, prefix, prefix_len) != 0) {
        return 0;
    }

    return key[prefix_len] != '\0';
}

static int info_has_key(const PackageInfo *info, const char *key) {
    size_t i;

    if (info == NULL || is_empty_string(key)) {
        return 0;
    }

    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError add_info_field(PackageInfo *info, const char *key, const char *value) {
    CupError err;

    if (info == NULL || is_empty_string(key) || is_empty_string(value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (info_has_key(info, key)) {
        fprintf(stderr, "Error: duplicate info key '%s'.\n", key);
        return CUP_ERR_VALIDATION;
    }

    if (info->count >= MAX_INFO_FIELDS) {
        fprintf(stderr, "Error: info file contains too many fields.\n");
        return CUP_ERR_VALIDATION;
    }

    err = checked_snprintf(info->fields[info->count].key, sizeof(info->fields[info->count].key), "%s", key);
    if (err != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    err = checked_snprintf(info->fields[info->count].value, sizeof(info->fields[info->count].value), "%s", value);
    if (err != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    info->count++;
    return CUP_OK;
}

const char *get_info_value(const PackageInfo *info, const char *key) {
    size_t i;

    if (info == NULL || is_empty_string(key)) {
        return NULL;
    }

    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return info->fields[i].value;
        }
    }

    return NULL;
}

static CupError parse_info_line(PackageInfo *info, char *line) {
    CupError err;
    char key[MAX_INFO_KEY_LEN];
    char value[MAX_INFO_VALUE_LEN];

    err = split_key_value(line, key, sizeof(key), value, sizeof(value));
    if (err != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    err = add_info_field(info, key, value);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError info_load(PackageInfo *info, const char *path) {
    CupError err;
    FILE *file;
    char line[MAX_INFO_LINE_LEN];
    size_t line_number;

    if (info == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(info, 0, sizeof(*info));

    file = fopen(path, "r");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    line_number = 0;

    while (1) {
        int has_line;

        err = read_text_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            if (err == CUP_ERR_BUFFER_TOO_SMALL) {
                fprintf(stderr, "Error: info file line %zu is too long.\n", line_number);
            } else {
                fprintf(stderr, "Error: could not read info file line.\n");
            }

            fclose(file);
            return CUP_ERR_VALIDATION;
        }

        if (!has_line) {
            break;
        }

        err = parse_info_line(info, line);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid info file line %zu.\n", line_number);
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
    }

    if (fclose(file) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

const PackageInfoField *next_info_field(const PackageInfo *info, const char *prefix, size_t *cursor) {
    size_t i;

    if (info == NULL || is_empty_string(prefix) || cursor == NULL) {
        return NULL;
    }

    i = *cursor;

    while (i < info->count) {
        const PackageInfoField *field;

        field = &info->fields[i];
        i++;

        if (info_key_has_prefix(field->key, prefix)) {
            *cursor = i;
            return field;
        }
    }

    *cursor = i;
    return NULL;
}

size_t count_info_fields(const PackageInfo *info, const char *prefix) {
    size_t count;
    size_t cursor;

    if (info == NULL || is_empty_string(prefix)) {
        return 0;
    }

    count = 0;
    cursor = 0;

    while (next_info_field(info, prefix, &cursor) != NULL) {
        count++;
    }

    return count;
}