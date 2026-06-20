#ifndef CUP_TEXT_H
#define CUP_TEXT_H

#include <stddef.h>
#include <stdio.h>

#include "error.h"

/* Read the next non-empty, non-comment line from a text file. */
CupError text_read_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number);

/* Split a line at the first '=' and trim both fields. */
CupError text_parse_key_value(char *line, char *key, size_t key_size, char *value, size_t value_size);

#endif /* CUP_TEXT_H */
