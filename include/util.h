#ifndef CUP_UTIL_H
#define CUP_UTIL_H

#include <stddef.h>

#include "error.h"

int is_empty_string(const char *value);
void trim_line_end(char *line);
char *trim_spaces(char *text);
CupError parse_key_value_line(char *line, char **key, char **value, int *has_pair);
CupError read_key_value_field(char *buffer, size_t size, const char *filename, const char *field, int *found);
CupError split_once(const char *input, char separator, char *left, size_t left_size, char *right, size_t right_size);
CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif /* CUP_UTIL_H */