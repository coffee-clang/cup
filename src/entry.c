#include "entry.h"

#include "constants.h"
#include "util.h"

#include <string.h>

// ENTRY FORMAT
int entry_is_stable(const char *release) {
    if (is_empty_string(release)) {
        return 0;
    }

    return strcmp(release, "stable") == 0;
}

CupError entry_parse(const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    char buffer[MAX_ENTRY_LEN];
    SplitOutput outputs[2];
    CupError err;

    if (is_empty_string(entry) || tool == NULL || tool_size == 0 ||
        release == NULL || release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, sizeof(buffer), "%s", entry);
    if (err != CUP_OK) {
        return err;
    }

    outputs[0] = (SplitOutput){tool, tool_size};
    outputs[1] = (SplitOutput){release, release_size};

    err = split_exact(buffer, '@', outputs, 2);
    if (err != CUP_OK || is_empty_string(tool) || is_empty_string(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return CUP_OK;
}

CupError entry_build(char *buffer, size_t size, const char *tool, const char *release) {
    if (buffer == NULL || size == 0 || is_empty_string(tool) || is_empty_string(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "%s@%s", tool, release);
}
