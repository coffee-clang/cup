#ifndef CUP_UTIL_H
#define CUP_UTIL_H

#include <stddef.h>

#include "error.h"

void print_step(const char *message);
int is_empty_string(const char *value);
CupError split_once(const char *input, char separator, char *left, size_t left_size, char *right, size_t right_size);
CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif /* CUP_UTIL_H */