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

int text_parse_uint(const char *value, unsigned maximum, unsigned *result) {
    unsigned parsed = 0;
    size_t i;

    if (text_is_empty(value) || result == NULL ||
        (value[0] == '0' && value[1] != '\0')) {
        return 0;
    }
    for (i = 0; value[i] != '\0'; ++i) {
        unsigned digit;

        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
        digit = (unsigned)(value[i] - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10u) {
            return 0;
        }
        parsed = parsed * 10u + digit;
    }
    *result = parsed;
    return 1;
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

/* Canonical in-memory document readers own persistent text parsing. */
CupError text_document_reader_init(TextDocumentReader *reader,
                                   const unsigned char *data,
                                   size_t size) {
    size_t i;

    if (reader == NULL || data == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (data[size - 1] != '\n') {
        return CUP_ERR_VALIDATION;
    }
    for (i = 0; i < size; ++i) {
        unsigned char byte = data[i];
        if (byte != '\n' && (byte < 0x20u || byte > 0x7eu)) {
            return CUP_ERR_VALIDATION;
        }
    }
    reader->data = data;
    reader->size = size;
    reader->offset = 0;
    reader->line_number = 0;
    return CUP_OK;
}

CupError text_document_read_raw_line(TextDocumentReader *reader,
                                     char *buffer,
                                     size_t size,
                                     int *has_line) {
    size_t start;
    size_t length;

    if (reader == NULL || buffer == NULL || size < 2 || has_line == NULL ||
        reader->data == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_line = 0;
    if (reader->offset >= reader->size) {
        return CUP_OK;
    }
    start = reader->offset;
    while (reader->offset < reader->size && reader->data[reader->offset] != '\n') {
        reader->offset++;
    }
    if (reader->offset >= reader->size) {
        return CUP_ERR_VALIDATION;
    }
    length = reader->offset - start;
    reader->offset++;
    reader->line_number++;
    if (length >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, reader->data + start, length);
    buffer[length] = '\0';
    *has_line = 1;
    return CUP_OK;
}

CupError text_document_read_line(TextDocumentReader *reader,
                                 char *buffer,
                                 size_t size,
                                 int *has_line) {
    CupError err;

    if (reader == NULL || buffer == NULL || size < 2 || has_line == NULL ||
        reader->data == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    while (1) {
        char *trimmed;

        err = text_document_read_raw_line(reader, buffer, size, has_line);
        if (err != CUP_OK || !*has_line) {
            return err;
        }
        trimmed = text_trim(buffer);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed != buffer) {
            memmove(buffer, trimmed, strlen(trimmed) + 1);
        }
        return CUP_OK;
    }
}

/* Key/value parsing trims surrounding whitespace and rejects empty keys or values. */
CupError text_parse_key_value(char *line,
                              char *key,
                              size_t key_size,
                              char *value,
                              size_t value_size) {
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
