#include "text.h"

#include "util.h"

#include <limits.h>
#include <string.h>

// LINE READING
static void trim_line_end(char *line) {
    size_t length;

    length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
}

CupError text_read_line(FILE *file, char *buffer, size_t size, int *has_line, size_t *line_number) {
    char *text;
    size_t len;
    int ch;

    if (file == NULL || buffer == NULL || size < 2 || size > INT_MAX || has_line == NULL || line_number == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *has_line = 0;

    while (1) {
        if (fgets(buffer, (int)size, file) == NULL) {
            if (ferror(file)) {
                return CUP_ERR_FILESYSTEM;
            }
            return CUP_OK;
        }

        (*line_number)++;
        len = strlen(buffer);

        if (len > 0 && buffer[len - 1] != '\n' && buffer[len - 1] != '\r' && !feof(file)) {
            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
            return CUP_ERR_BUFFER_TOO_SMALL;
        }

        trim_line_end(buffer);
        text = trim_spaces(buffer);

        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }

        if (text != buffer) {
            memmove(buffer, text, strlen(text) + 1);
        }

        *has_line = 1;
        return CUP_OK;
    }
}

// KEY/VALUE PARSING
CupError text_parse_key_value(char *line, char *key, size_t key_size, char *value, size_t value_size) {
    CupError err;
    char *separator;
    char *trimmed_key;
    char *trimmed_value;

    if (line == NULL || key == NULL || key_size == 0 || value == NULL || value_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    separator = strchr(line, '=');
    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *separator = '\0';
    trimmed_key = trim_spaces(line);
    trimmed_value = trim_spaces(separator + 1);

    if (is_empty_string(trimmed_key) || is_empty_string(trimmed_value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(key, key_size, "%s", trimmed_key);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(value, value_size, "%s", trimmed_value);
    return err;
}
