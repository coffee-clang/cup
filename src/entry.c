#include "entry.h"

#include "constants.h"
#include "text.h"

#include <string.h>

// ENTRY FORMAT
int entry_is_stable(const char *release) {
    if (text_is_empty(release)) {
        return 0;
    }

    return strcmp(release, "stable") == 0;
}

CupError entry_parse(const char *entry, char *tool, size_t tool_size,
    char *release, size_t release_size) {
    char buffer[MAX_ENTRY_LEN];
    TextBuffer outputs[2];
    CupError err;

    if (text_is_empty(entry) || tool == NULL || tool_size == 0 ||
        release == NULL || release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_copy(buffer, sizeof(buffer), entry);
    if (err != CUP_OK) {
        return err;
    }

    outputs[0] = (TextBuffer){.data = tool, .capacity = tool_size};
    outputs[1] = (TextBuffer){.data = release, .capacity = release_size};

    err = text_split_exact(buffer, '@', outputs, 2);
    if (err != CUP_OK || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return CUP_OK;
}

CupError entry_build(char *buffer, size_t size, const char *tool, const char *release) {
    if (buffer == NULL || size == 0 || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return text_format(buffer, size, "%s@%s", tool, release);
}
