#include "entry.h"

#include "constants.h"
#include "util.h"

#include <string.h>

int is_stable_release(const char *release) {
    if (is_empty_string(release)) {
        return 0;
    }

    return strcmp(release, "stable") == 0;
}

CupError parse_entry(const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    CupError err;
    char entry_copy[MAX_ENTRY_LEN];
    SplitOutput outputs[2];

    if (is_empty_string(entry) || tool == NULL || tool_size == 0 || release == NULL || release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(entry_copy, sizeof(entry_copy), "%s", entry);
    if (err != CUP_OK) {
        return err;
    }

    outputs[0].buffer = tool;
    outputs[0].size = tool_size;
    outputs[1].buffer = release;
    outputs[1].size = release_size;

    err = split_exact(entry_copy, '@', outputs, 2);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError build_entry(char *buffer, size_t size, const char *tool, const char *release) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(tool) || is_empty_string(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s@%s", tool, release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}