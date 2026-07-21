/*
 * Implements bounded path construction and syntax validation for identifiers and safe relative
 * package paths.
 */

#include "path.h"

#include "text.h"

#include <ctype.h>
#include <string.h>

/* Path building. */
CupError path_join(char *buffer, size_t size, const char *parent, const char *child) {
    if (buffer == NULL || size == 0 || text_is_empty(parent) || text_is_empty(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return text_format(buffer, size, "%s/%s", parent, child);
}

CupError path_join_safe_relative(char *buffer, size_t size, const char *parent, const char *child) {
    if (buffer == NULL || size == 0 || text_is_empty(parent)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!path_is_safe_relative(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return path_join(buffer, size, parent, child);
}

const char *path_last_segment(const char *path) {
    const char *slash;
    const char *backslash;

    if (path == NULL) {
        return NULL;
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');

    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }

    return slash == NULL ? path : slash + 1;
}

/* Path validation. */
static int equals_ignore_case_n(const char *left, const char *right, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return 0;
        }
    }

    return right[length] == '\0';
}

static int is_windows_reserved_name(const char *value) {
    static const char *const reserved[] = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const char *dot;
    size_t base_length;
    size_t i;

    if (text_is_empty(value)) {
        return 0;
    }

    dot = strchr(value, '.');
    base_length = dot == NULL ? strlen(value) : (size_t)(dot - value);

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        if (strlen(reserved[i]) == base_length &&
            equals_ignore_case_n(value, reserved[i], base_length)) {
            return 1;
        }
    }

    return 0;
}

int path_is_safe_segment(const char *value) {
    const unsigned char *cursor;
    size_t length;

    if (text_is_empty(value)) {
        return 0;
    }

    length = strlen(value);
    if ((length == 1 && value[0] == '.') || (length == 2 && value[0] == '.' && value[1] == '.')) {
        return 0;
    }

    for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        /* Package and managed path segments use printable ASCII only. */
        if (*cursor < 0x21 || *cursor > 0x7e || *cursor == '/' || *cursor == '\\' ||
            *cursor == ':' || *cursor == '*' || *cursor == '?' || *cursor == '"' ||
            *cursor == '<' || *cursor == '>' || *cursor == '|') {
            return 0;
        }
    }

    if (value[length - 1] == '.' || value[length - 1] == ' ') {
        return 0;
    }

    return !is_windows_reserved_name(value);
}

int path_is_safe_identifier(const char *value) {
    const unsigned char *cursor;

    if (!path_is_safe_segment(value) || !isalnum((unsigned char)value[0])) {
        return 0;
    }

    for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        if (isalnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '+' ||
            *cursor == '-') {
            continue;
        }

        return 0;
    }

    return 1;
}

int path_is_safe_relative(const char *path) {
    const char *segment;
    const char *cursor;
    char part[256];
    size_t length;

    if (text_is_empty(path)) {
        return 0;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return 0;
    }

    if (path[0] != '\0' && path[1] == ':') {
        return 0;
    }

    if (strchr(path, '\\') != NULL || strchr(path, ':') != NULL) {
        return 0;
    }

    segment = path;
    cursor = path;

    while (1) {
        if (*cursor == '/' || *cursor == '\0') {
            length = (size_t)(cursor - segment);
            if (length == 0 || length >= sizeof(part)) {
                return 0;
            }

            memcpy(part, segment, length);
            part[length] = '\0';

            if (!path_is_safe_segment(part)) {
                return 0;
            }

            if (*cursor == '\0') {
                break;
            }

            segment = cursor + 1;
        }

        cursor++;
    }

    return 1;
}
