#ifndef CUP_TEXT_H
#define CUP_TEXT_H

/*
 * Module contract: Bounded text and key/value parsing utilities for project
 * text formats. Functions never allocate and always preserve destination
 * bounds supplied by the caller.
 */

#include <stddef.h>
#include <stdio.h>

#include "error.h"

/* Caller-owned output slot used by exact splitting. */
typedef struct {
    char *data;
    size_t capacity;
} TextBuffer;

/* Basic non-owning string inspection and in-place trimming. */
int text_is_empty(const char *value);
char *text_trim(char *value);

/* Bounded copy and printf-style formatting. */
CupError text_copy(char *buffer, size_t size, const char *source);
CupError text_copy_lower_ascii(char *buffer, size_t size, const char *source);
#if defined(__GNUC__) || defined(__clang__)
CupError text_format(char *buffer, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#else
CupError text_format(char *buffer, size_t size, const char *format, ...);
#endif

/* Split into exactly output_count nonempty bounded fields. */
CupError text_split_exact(char *input, char separator, TextBuffer *outputs, size_t output_count);

/*
 * Read the next nonempty data line, skipping blank and comment lines. The
 * line counter advances for every complete logical line that is consumed,
 * including skipped or rejected lines.
 */
CupError text_read_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number);

/* Parse one nonempty key=value line into bounded caller-owned buffers. */
CupError text_parse_key_value(
    char *line, char *key, size_t key_size, char *value, size_t value_size);

#endif /* CUP_TEXT_H */
