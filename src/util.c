#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// STRING HELPERS
int is_empty_string(const char *value) {
    return value == NULL || value[0] == '\0';
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

// STRING SPLITTING
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

// SAFE FORMATTING
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
