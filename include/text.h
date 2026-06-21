#ifndef CUP_TEXT_H
#define CUP_TEXT_H

#include <stddef.h>
#include <stdio.h>

#include "error.h"

typedef struct {
    char *data;
    size_t capacity;
} TextBuffer;

int text_is_empty(const char *value);
char *text_trim(char *value);
CupError text_format(char *buffer, size_t size, const char *format, ...);
CupError text_split_exact(char *input, char separator,
    TextBuffer *outputs, size_t output_count);
CupError text_read_line(FILE *file, char *buffer, size_t size,
    int *has_line, size_t *line_number);
CupError text_parse_key_value(char *line, char *key, size_t key_size,
    char *value, size_t value_size);

#endif /* CUP_TEXT_H */
