/*
 * Implements bounded path construction and syntax validation for identifiers and safe relative
 * package paths.
 */

#include "path.h"

#include "constants.h"
#include "text.h"

#include <ctype.h>
#include <string.h>

#if defined(_WIN32)
static int is_path_separator(char value) {
    return value == '/' || value == '\\';
}

static unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int ascii_equal_ignore_case(char left, char right) {
    return ascii_lower((unsigned char)left) == ascii_lower((unsigned char)right);
}

static int is_drive_root(const char *path, size_t length) {
    return length == 3 && isalpha((unsigned char)path[0]) && path[1] == ':' && path[2] == '/';
}
#endif

CupError path_normalize(char *path) {
    if (text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

#if defined(_WIN32)
    {
        size_t length = strlen(path);
        size_t read = 0;
        size_t write = 0;

        if (length >= 8 && is_path_separator(path[0]) && is_path_separator(path[1]) &&
            path[2] == '?' && is_path_separator(path[3]) &&
            ascii_equal_ignore_case(path[4], 'u') && ascii_equal_ignore_case(path[5], 'n') &&
            ascii_equal_ignore_case(path[6], 'c') && is_path_separator(path[7])) {
            path[write++] = '/';
            path[write++] = '/';
            read = 8;
        } else if (length >= 4 && is_path_separator(path[0]) && is_path_separator(path[1]) &&
                   path[2] == '?' && is_path_separator(path[3])) {
            read = 4;
        } else if (length >= 4 && is_path_separator(path[0]) && is_path_separator(path[1]) &&
                   path[2] == '.' && is_path_separator(path[3])) {
            return CUP_ERR_INVALID_INPUT;
        } else if (length >= 2 && is_path_separator(path[0]) && is_path_separator(path[1])) {
            path[write++] = '/';
            path[write++] = '/';
            read = 2;
        }

        while (read < length) {
            char value = path[read++];

            if (is_path_separator(value)) {
                if (write > 0 && path[write - 1] == '/') {
                    continue;
                }
                value = '/';
            }
            path[write++] = value;
        }

        while (write > 1 && path[write - 1] == '/' && !is_drive_root(path, write) &&
               !(write == 2 && path[0] == '/' && path[1] == '/')) {
            write--;
        }
        path[write] = '\0';
    }
#endif

    return CUP_OK;
}

int path_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return 0;
    }

#if defined(_WIN32)
    {
        char normalized_left[MAX_PATH_LEN];
        char normalized_right[MAX_PATH_LEN];
        const unsigned char *left_cursor;
        const unsigned char *right_cursor;

        if (text_copy(normalized_left, sizeof(normalized_left), left) != CUP_OK ||
            text_copy(normalized_right, sizeof(normalized_right), right) != CUP_OK ||
            path_normalize(normalized_left) != CUP_OK ||
            path_normalize(normalized_right) != CUP_OK) {
            return 0;
        }

        left_cursor = (const unsigned char *)normalized_left;
        right_cursor = (const unsigned char *)normalized_right;
        while (*left_cursor != '\0' && *right_cursor != '\0') {
            if (ascii_lower(*left_cursor) != ascii_lower(*right_cursor)) {
                return 0;
            }
            left_cursor++;
            right_cursor++;
        }
        return *left_cursor == *right_cursor;
    }
#else
    return strcmp(left, right) == 0;
#endif
}

/* Bounded path composition that preserves the host's internal representation. */
CupError path_join(char *buffer, size_t size, const char *parent, const char *child) {
    CupError err;

    if (buffer == NULL || size == 0 || text_is_empty(parent) || text_is_empty(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_format(buffer, size, "%s/%s", parent, child);
    return err == CUP_OK ? path_normalize(buffer) : err;
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

CupError path_parent(char *buffer, size_t size, const char *path) {
    char *separator;

    if (text_is_empty(path) || buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (text_copy(buffer, size, path) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (path_normalize(buffer) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    separator = strrchr(buffer, '/');
    if (separator == NULL) {
        return text_copy(buffer, size, ".");
    }

#if defined(_WIN32)
    if (separator == buffer + 2 && buffer[1] == ':') {
        separator[1] = '\0';
        return CUP_OK;
    }
#endif
    if (separator == buffer) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return CUP_OK;
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

/* Reject separators, traversal and platform aliases before values become path segments. */
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
