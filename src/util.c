#include "util.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void print_step(const char *message) {
    if (is_empty_string(message)) {
        return;
    }

    printf("==> %s\n", message);
    fflush(stdout);
}

int is_empty_string(const char *value) {
    return value == NULL || value[0] == '\0';
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