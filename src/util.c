#include "util.h"

#include <stdio.h>
#include <stdarg.h>

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (buffer == NULL || format == NULL || size == 0) {
        fprintf(stderr, "Error: invalid snprintf arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "Error: formatted string is too long.\n");
        return CUP_ERR_FS;
    }

    return CUP_OK;
}