#include "util.h"

#include <stdarg.h>
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

CupError read_text_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number) {
    char *text;
    size_t len;
    int ch;

    if (file == NULL || buffer == NULL || size < 2 || has_line == NULL || line_number == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *has_line = 0;

    while (1) {
        if (fgets(buffer, size, file) == NULL) {
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
        text = trim_spaces(buffer);

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

CupError split_exact(char *input, char separator, SplitOutput *outputs, size_t output_count) {
    CupError err;
    char *cursor;
    char *part;
    size_t count;

    if (is_empty_string(input) || separator == '\0' || outputs == NULL || output_count == 0) {
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

        part = trim_spaces(part);
        if (is_empty_string(part)) {
            return CUP_ERR_INVALID_INPUT;
        }

        if (outputs[count].buffer == NULL || outputs[count].size == 0) {
            return CUP_ERR_INVALID_INPUT;
        }

        err = checked_snprintf(outputs[count].buffer, outputs[count].size, "%s", part);
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

CupError split_key_value(char *line, char *key, size_t key_size, char *value, size_t value_size) {
    CupError err;
    SplitOutput outputs[2];

    if (line == NULL || key == NULL || key_size == 0 || value == NULL || value_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    outputs[0].buffer = key;
    outputs[0].size = key_size;
    outputs[1].buffer = value;
    outputs[1].size = value_size;

    err = split_exact(line, '=', outputs, 2);
    return err;
}

CupError split_list_contains(char *input, char separator, const char *expected, int *contains) {
    char *cursor;
    char *part;

    if (is_empty_string(input) || separator == '\0' || is_empty_string(expected) || contains == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *contains = 0;
    cursor = input;

    while (1) {
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

        part = trim_spaces(part);
        if (is_empty_string(part)) {
            return CUP_ERR_INVALID_INPUT;
        }

        if (strcmp(part, expected) == 0) {
            *contains = 1;
        }

        if (cursor == NULL) {
            break;
        }
    }

    return CUP_OK;
}

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (buffer == NULL || size == 0 || is_empty_string(format)) {
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
