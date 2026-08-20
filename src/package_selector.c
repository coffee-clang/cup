/*
 * Validates symbolic or concrete package selectors and preserves the public tool@release form.
 */

#include "package_selector.h"

#include "constants.h"
#include "path.h"
#include "text.h"

#include <string.h>

_Static_assert(MAX_SELECTOR_LEN >= 2 * MAX_IDENTIFIER_LEN,
               "selector capacity must hold two maximum identifiers and '@'");

/* Parse symbolic or concrete tool selectors without consulting the catalog. */
static CupError selector_init(PackageSelector *selector, const char *tool, const char *release) {
    const unsigned char *cursor;
    CupError err;

    if (selector == NULL || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!path_is_safe_identifier(tool)) {
        return CUP_ERR_INVALID_TOOL;
    }
    for (cursor = (const unsigned char *)tool; *cursor != '\0'; ++cursor) {
        if (*cursor >= 'A' && *cursor <= 'Z') {
            return CUP_ERR_INVALID_TOOL;
        }
    }

    if (!package_release_is_stable(release)) {
        err = package_release_validate_concrete(release);
        if (err != CUP_OK) {
            return err;
        }
    }

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
    memset(selector, 0, sizeof(*selector));

    err = package_selector_parse_parts(text, tool, sizeof(tool), release, sizeof(release));
    if (err != CUP_OK) {
        return err;
    }

    return selector_init(selector, tool, release);
}

/* Build and split canonical <tool>@<release> strings used at CLI and persistence boundaries. */
int package_release_is_stable(const char *release) {
    if (text_is_empty(release)) {
        return 0;
    }

    return strcmp(release, "stable") == 0;
}

CupError package_release_validate_concrete(const char *release) {
    char canonical[MAX_IDENTIFIER_LEN];
    CupError err;

    if (text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = text_copy_lower_ascii(canonical, sizeof(canonical), release);
    if (err != CUP_OK) {
        return err;
    }
    if (strcmp(canonical, release) != 0 || package_release_is_stable(release) ||
        !path_is_safe_identifier(release)) {
        return CUP_ERR_INVALID_RELEASE;
    }
    return CUP_OK;
}

CupError package_selector_parse_parts(
    const char *text, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *separator;
    size_t release_length;
    size_t text_length;
    size_t tool_length;

    if (text_is_empty(text) || tool == NULL || tool_size == 0 || release == NULL ||
        release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    text_length = strlen(text);
    if (text_length >= MAX_SELECTOR_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    separator = strchr(text, '@');
    if (separator == NULL || separator == text || separator[1] == '\0' ||
        strchr(separator + 1, '@') != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    tool_length = (size_t)(separator - text);
    release_length = text_length - tool_length - 1u;
    if (tool_length >= tool_size || release_length >= release_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    memmove(tool, text, tool_length);
    tool[tool_length] = '\0';
    memmove(release, separator + 1, release_length + 1u);

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
