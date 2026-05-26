#ifndef CUP_UTIL_H
#define CUP_UTIL_H

#include <stddef.h>
#include <stdio.h>

#include "error.h"

typedef struct {
    char *buffer;
    size_t size;
} SplitOutput;

int is_empty_string(const char *value);
void trim_line_end(char *line);
char *trim_spaces(char *text);
CupError read_text_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number);
CupError split_exact(char *input, char separator, SplitOutput *outputs, size_t output_count);
CupError split_key_value(char *line, char *key, size_t key_size, char *value, size_t value_size);
CupError split_list_contains(char *input, char separator, const char *expected, int *contains);
CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif /* CUP_UTIL_H */
