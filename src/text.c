/*
 * Provides bounded text-copy, formatting, line-reading and exact key/value parsing helpers
 * shared by persistent text formats.
 */

#include "text.h"

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

/* Destructive bounded splitting used only after callers own a mutable buffer. */
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

        err = text_copy(outputs[count].data, outputs[count].capacity, part);
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

/* Copy and format helpers always terminate successful outputs and report truncation. */
CupError text_copy(char *buffer, size_t size, const char *source) {
    size_t length;

    if (buffer == NULL || size == 0 || source == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    length = strlen(source);
    if (length >= size) {
        fprintf(stderr, "Error: copied string is too long.\n");
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    memmove(buffer, source, length + 1);
    return CUP_OK;
}

CupError text_copy_lower_ascii(char *buffer, size_t size, const char *source) {
    size_t i;
    CupError err = text_copy(buffer, size, source);

    if (err != CUP_OK) {
        return err;
    }
    for (i = 0; buffer[i] != '\0'; ++i) {
        if (buffer[i] >= 'A' && buffer[i] <= 'Z') {
            buffer[i] = (char)(buffer[i] - 'A' + 'a');
        }
    }
    return CUP_OK;
}

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

/* Line reader distinguishes EOF, overlong records and underlying I/O failure. */
static int is_allowed_text_byte(unsigned char byte) {
    return byte == '\t' || byte >= 32;
}

CupError text_read_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number) {
    size_t length;
    int byte;
    int line_too_long;

    if (file == NULL || buffer == NULL || size < 2 || has_line == NULL || line_number == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *has_line = 0;

    while (1) {
        length = 0;
        line_too_long = 0;

        while ((byte = fgetc(file)) != EOF) {
            unsigned char value = (unsigned char)byte;

            if (value == '\n') {
                break;
            }
            if (value == '\r') {
                int next = fgetc(file);
                if (next != '\n' && next != EOF) {
                    ungetc(next, file);
                }
                break;
            }
            if (value == '\0' || !is_allowed_text_byte(value)) {
                while ((byte = fgetc(file)) != EOF && byte != '\n') {
                }
                (*line_number)++;
                return CUP_ERR_INVALID_INPUT;
            }
            if (length + 1 < size) {
                buffer[length++] = (char)value;
            } else {
                line_too_long = 1;
            }
        }

        if (byte == EOF && ferror(file)) {
            return CUP_ERR_FILESYSTEM;
        }
        if (byte == EOF && length == 0 && !line_too_long) {
            return CUP_OK;
        }

        (*line_number)++;
        if (line_too_long) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }

        buffer[length] = '\0';
        {
            char *text = text_trim(buffer);

            if (text[0] == '\0' || text[0] == '#') {
                if (byte == EOF) {
                    return CUP_OK;
                }
                continue;
            }

            if (text != buffer) {
                memmove(buffer, text, strlen(text) + 1);
            }
        }

        *has_line = 1;
        return CUP_OK;
    }
}

/* Strict key/value parsing rejects empty sides, embedded whitespace and trailing data. */
CupError text_parse_key_value(
    char *line, char *key, size_t key_size, char *value, size_t value_size) {
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

    err = text_copy(key, key_size, trimmed_key);
    if (err != CUP_OK) {
        return err;
    }

    err = text_copy(value, value_size, trimmed_value);
    return err;
}
