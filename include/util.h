#ifndef CUP_UTIL_H
#define CUP_UTIL_H

#include <stddef.h>

#include "error.h"

typedef struct {
    char *buffer;
    size_t size;
} SplitOutput;

/* Generic string and formatting helpers. */
int is_empty_string(const char *value);
char *trim_spaces(char *text);
CupError split_exact(char *input, char separator, SplitOutput *outputs, size_t output_count);
CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif /* CUP_UTIL_H */
