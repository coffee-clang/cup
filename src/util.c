#include "util.h"

#include "constants.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int is_empty_string(const char *value) {
    return value == NULL || value[0] == '\0';
}

void trim_line_end(char *line) {
    size_t len;

    if (line == NULL) {
        return;
    }

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

char *trim_spaces(char *text) {
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (*text == ' ' || *text == '\t') {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    return text;
}

CupError parse_key_value_line(char *line, char **key, char **value, int *has_pair) {
    char *equals;
    char *line_key;
    char *line_value;

    if (line == NULL || key == NULL || value == NULL || has_pair == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *key = NULL;
    *value = NULL;
    *has_pair = 0;

    trim_line_end(line);

    line_key = trim_spaces(line);
    if (line_key[0] == '\0' || line_key[0] == '#') {
        return CUP_OK;
    }

    equals = strchr(line_key, '=');
    if (equals == NULL) {
        return CUP_OK;
    }

    *equals = '\0';

    line_key = trim_spaces(line_key);
    line_value = trim_spaces(equals + 1);

    if (line_key[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    *key = line_key;
    *value = line_value;
    *has_pair = 1;

    return CUP_OK;
}

CupError read_key_value_field(char *buffer, size_t size, const char *filename, const char *field, int *found) {
    CupError err;
    FILE *file;
    char line[MAX_MANIFEST_LINE_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(filename) || is_empty_string(field) || found == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    buffer[0] = '\0';
    *found = 0;

    file = fopen(filename, "r");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *key;
        char *value;
        int has_pair;

        err = parse_key_value_line(line, &key, &value, &has_pair);
        if (err != CUP_OK) {
            fclose(file);
            return err;
        }

        if (!has_pair) {
            continue;
        }

        if (strcmp(key, field) == 0) {
            err = checked_snprintf(buffer, size, "%s", value);
            fclose(file);
            if (err != CUP_OK) {
                return err;
            }

            *found = 1;
            return CUP_OK;
        }
    }

    if (fclose(file) != 0) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

CupError split_once(const char *input, char separator, char *left, size_t left_size, char *right, size_t right_size) {
    const char *separator_pos;
    size_t left_len;
    size_t right_len;

    if (left == NULL || right == NULL ||
        left_size == 0 || right_size == 0 || is_empty_string(input)) {
        return CUP_ERR_INVALID_INPUT;
    }

    separator_pos = strchr(input, separator);
    if (separator_pos == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strchr(separator_pos + 1, separator) != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    left_len = (size_t)(separator_pos - input);
    right_len = strlen(separator_pos + 1);

    if (left_len == 0 || right_len == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (left_len >= left_size || right_len >= right_size) {
        return CUP_ERR_INVALID_INPUT;
    }

    memcpy(left, input, left_len);
    left[left_len] = '\0';

    memcpy(right, separator_pos + 1, right_len);
    right[right_len] = '\0';

    return CUP_OK;
}

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (buffer == NULL || size == 0 || is_empty_string(format)) {
        fprintf(stderr, "Error: invalid snprintf arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "Error: formatted string is too long.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}