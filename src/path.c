#include "path.h"

#include "util.h"

#include <string.h>

// PATH BUILDING
CupError path_join(char *buffer, size_t size, const char *parent, const char *child) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(parent) || is_empty_string(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s/%s", parent, child);
    return err;
}

CupError path_join_safe_relative(char *buffer, size_t size, const char *parent, const char *child) {
    CupError err;
    
    if (buffer == NULL || size == 0 || is_empty_string(parent)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!path_is_safe_relative(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(buffer, size, parent, child);
    return err;
}

// PATH VALIDATION
static int is_path_separator(char value) {
    return value == '/' || value == '\\';
}

int path_has_parent_ref(const char *path) {
    const char *start;
    const char *end;
    size_t len;

    if (path == NULL) {
        return 1;
    }

    start = path;

    while (*start != '\0') {
        end = start;

        while (*end != '\0' && !is_path_separator(*end)) {
            end++;
        }

        len = (size_t)(end - start);

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return 1;
        }

        if (*end == '\0') {
            break;
        }

        start = end + 1;
    }

    return 0;
}

int path_is_safe_relative(const char *path) {
    if (is_empty_string(path)) {
        return 0;
    }

    if (path[0] == '/' || path[0] == '\\') {
        return 0;
    }

    if (path[0] != '\0' && path[1] == ':') {
        return 0;
    }

    if (path_has_parent_ref(path)) {
        return 0;
    }

    return 1;
}