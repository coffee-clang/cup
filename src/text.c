#include "text.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


int text_is_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}

char *text_trim(char *text) {
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

// STRING SPLITTING
CupError text_split_exact(char *input, char separator, TextBuffer *outputs, size_t output_count) {
    CupError err;
    char *cursor;
    char *part;
    size_t count;

    if (text_is_empty(input) || separator == '\0' || outputs == NULL || output_count == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = input;
    count = 0;

    while (1) {
        if (count >= output_count) {
            return CUP_ERR_INVALID_INPUT;
        }

        part = cursor;
        while (*cursor != '\0' && *cursor != separator) {
            cursor++;
        }

        if (*cursor == separator) {
            *cursor = '\0';
            cursor++;
        } else {
            cursor = NULL;
        }

        part = text_trim(part);
        if (text_is_empty(part)) {
            return CUP_ERR_INVALID_INPUT;
        }

        if (outputs[count].data == NULL || outputs[count].capacity == 0) {
            return CUP_ERR_INVALID_INPUT;
        }

        err = text_format(outputs[count].data, outputs[count].capacity, "%s", part);
        if (err != CUP_OK) {
            return err;
        }

        count++;

        if (cursor == NULL) {
            break;
        }
    }

    if (count != output_count) {
        return CUP_ERR_INVALID_INPUT;
    }

    return CUP_OK;
}

// SAFE FORMATTING
CupError text_format(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (buffer == NULL || size == 0 || text_is_empty(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0) {
        fprintf(stderr, "Error: could not format string.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if ((size_t)written >= size) {
        fprintf(stderr, "Error: formatted string is too long.\n");
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

// LINE READING

static void trim_line_end(char *line) {
    size_t length;

    length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
}

CupError text_read_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number) {
    char *text;
    size_t len;
    int ch;

    if (file == NULL || buffer == NULL || size < 2 || size > INT_MAX ||
        has_line == NULL || line_number == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *has_line = 0;

    while (1) {
        if (fgets(buffer, (int)size, file) == NULL) {
            if (ferror(file)) {
                return CUP_ERR_FILESYSTEM;
            }
            return CUP_OK;
        }

        (*line_number)++;
        len = strlen(buffer);

        if (len > 0 && buffer[len - 1] != '\n' && buffer[len - 1] != '\r' && !feof(file)) {
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
            return CUP_ERR_BUFFER_TOO_SMALL;
        }

        trim_line_end(buffer);
        text = text_trim(buffer);

        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }

        if (text != buffer) {
            memmove(buffer, text, strlen(text) + 1);
        }

        *has_line = 1;
        return CUP_OK;
    }
}

// KEY/VALUE PARSING
CupError text_parse_key_value(char *line, char *key, size_t key_size,
    char *value, size_t value_size) {
    CupError err;
    char *separator;
    char *trimmed_key;
    char *trimmed_value;

    if (line == NULL || key == NULL || key_size == 0 || value == NULL || value_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    separator = strchr(line, '=');
    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *separator = '\0';
    trimmed_key = text_trim(line);
    trimmed_value = text_trim(separator + 1);

    if (text_is_empty(trimmed_key) || text_is_empty(trimmed_value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_format(key, key_size, "%s", trimmed_key);
    if (err != CUP_OK) {
        return err;
    }

    err = text_format(value, value_size, "%s", trimmed_value);
    return err;
}
