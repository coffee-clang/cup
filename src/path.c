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

// PATH VALIDATION
int path_has_parent_ref(const char *path) {
    const char *start;
    const char *slash;
    size_t len;

    if (path == NULL) {
        return 1;
    }

    start = path;

    while (*start != '\0') {
        slash = strchr(start, '/');

        if (slash == NULL) {
            len = strlen(start);
        } else {
            len = (size_t)(slash - start);
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return 1;
        }

        if (slash == NULL) {
            break;
        }

        start = slash + 1;
    }

    return 0;
}