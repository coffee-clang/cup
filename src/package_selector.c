/*
 * Validates symbolic or concrete package selectors and preserves the current tool@release
 * boundary representation.
 */

#include "package_selector.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include <string.h>

/* Package selectors. */
CupError package_selector_init(PackageSelector *selector, const char *tool, const char *release) {
    if (selector == NULL || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!path_is_safe_identifier(tool)) {
        return CUP_ERR_INVALID_TOOL;
    }
    if (!path_is_safe_identifier(release)) {
        return CUP_ERR_INVALID_RELEASE;
    }

    memset(selector, 0, sizeof(*selector));
    if (text_copy(selector->tool, sizeof(selector->tool), tool) != CUP_OK ||
        text_copy(selector->release, sizeof(selector->release), release) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

CupError package_selector_parse(PackageSelector *selector, const char *text) {
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
    CupError err;

    if (selector == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_selector_parse_parts(text, tool, sizeof(tool), release, sizeof(release));
    if (err != CUP_OK) {
        return err;
    }

    return package_selector_init(selector, tool, release);
}

CupError package_selector_format(const PackageSelector *selector, char *buffer, size_t size) {
    if (selector == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    return package_selector_format_parts(buffer, size, selector->tool, selector->release);
}

int package_selector_is_symbolic(const PackageSelector *selector) {
    return selector != NULL && package_release_is_stable(selector->release);
}

/* Entry format. */
int package_release_is_stable(const char *release) {
    if (text_is_empty(release)) {
        return 0;
    }

    return strcmp(release, "stable") == 0;
}

CupError package_selector_parse_parts(
    const char *text, char *tool, size_t tool_size, char *release, size_t release_size) {
    char buffer[MAX_SELECTOR_LEN];
    TextBuffer outputs[2];
    CupError err;

    if (text_is_empty(text) || tool == NULL || tool_size == 0 || release == NULL ||
        release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_copy(buffer, sizeof(buffer), text);
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

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release) {
    if (buffer == NULL || size == 0 || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return text_format(buffer, size, "%s@%s", tool, release);
}
