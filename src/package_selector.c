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
    CupError err;

    if (selector == NULL || text_is_empty(tool) || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_validate_canonical_identifier(tool, sizeof(selector->tool));
    if (err != CUP_OK) {
        return err == CUP_ERR_VALIDATION ? CUP_ERR_INVALID_TOOL : err;
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

typedef struct {
    const char *base_end;
    const char *revision;
} PackageVersionParts;

static int parse_numeric_dotted(const char *begin, const char *end) {
    const char *cursor = begin;

    if (begin == NULL || end == NULL || begin >= end) {
        return 0;
    }
    while (cursor < end) {
        const char *segment = cursor;

        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
        while (cursor < end && *cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
        if (cursor - segment > 1 && *segment == '0') {
            return 0;
        }
        if (cursor == end) {
            return 1;
        }
        if (*cursor != '.') {
            return 0;
        }
        cursor++;
        if (cursor == end) {
            return 0;
        }
    }
    return 0;
}

static int parse_revision(const char *value) {
    const char *cursor;

    if (value == NULL || value[0] < '1' || value[0] > '9') {
        return 0;
    }
    for (cursor = value + 1; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
    }
    return 1;
}

static int parse_package_version(const char *release, PackageVersionParts *parts) {
    const char *marker;
    size_t length;

    if (text_is_empty(release) || parts == NULL) {
        return 0;
    }
    length = strlen(release);
    if (length >= MAX_IDENTIFIER_LEN) {
        return 0;
    }
    marker = strstr(release, "-rev");
    if (marker != NULL && strstr(marker + 1, "-rev") != NULL) {
        return 0;
    }
    parts->base_end = marker != NULL ? marker : release + length;
    parts->revision = marker != NULL ? marker + 4 : NULL;
    if (!parse_numeric_dotted(release, parts->base_end)) {
        return 0;
    }
    return parts->revision == NULL || parse_revision(parts->revision);
}

CupError package_release_validate_concrete(const char *release) {
    PackageVersionParts parts;

    if (text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strlen(release) >= MAX_IDENTIFIER_LEN) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return parse_package_version(release, &parts) ? CUP_OK : CUP_ERR_INVALID_RELEASE;
}

static int compare_numeric_text(const char *left, size_t left_length,
                                const char *right, size_t right_length) {
    while (left_length > 1 && *left == '0') {
        left++;
        left_length--;
    }
    while (right_length > 1 && *right == '0') {
        right++;
        right_length--;
    }
    if (left_length != right_length) {
        return left_length < right_length ? -1 : 1;
    }
    {
        int result = memcmp(left, right, left_length);
        return result < 0 ? -1 : result > 0 ? 1 : 0;
    }
}

static int compare_base_versions(const char *left, const char *left_end,
                                 const char *right, const char *right_end) {
    const char *left_cursor = left;
    const char *right_cursor = right;

    while (1) {
        const char *left_segment = left_cursor;
        const char *right_segment = right_cursor;
        size_t left_length;
        size_t right_length;
        int result;

        while (left_cursor < left_end && *left_cursor != '.') left_cursor++;
        while (right_cursor < right_end && *right_cursor != '.') right_cursor++;
        left_length = (size_t)(left_cursor - left_segment);
        right_length = (size_t)(right_cursor - right_segment);
        result = compare_numeric_text(left_segment, left_length, right_segment, right_length);
        if (result != 0) {
            return result;
        }
        if (left_cursor == left_end || right_cursor == right_end) {
            if (left_cursor == left_end && right_cursor == right_end) return 0;
            return left_cursor == left_end ? -1 : 1;
        }
        left_cursor++;
        right_cursor++;
    }
}

CupError package_release_compare(const char *left, const char *right, int *result) {
    PackageVersionParts left_parts;
    PackageVersionParts right_parts;
    int compared;

    if (result == NULL || text_is_empty(left) || text_is_empty(right)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!parse_package_version(left, &left_parts) || !parse_package_version(right, &right_parts)) {
        return CUP_ERR_INVALID_RELEASE;
    }
    compared = compare_base_versions(left, left_parts.base_end, right, right_parts.base_end);
    if (compared == 0) {
        if (left_parts.revision == NULL || right_parts.revision == NULL) {
            compared = left_parts.revision == right_parts.revision ? 0
                       : left_parts.revision == NULL ? -1 : 1;
        } else {
            compared = compare_numeric_text(left_parts.revision, strlen(left_parts.revision),
                                            right_parts.revision, strlen(right_parts.revision));
        }
    }
    *result = compared;
    return CUP_OK;
}

CupError package_release_base(const char *release, char *base, size_t base_size) {
    PackageVersionParts parts;
    size_t length;

    if (base == NULL || base_size == 0 || text_is_empty(release)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!parse_package_version(release, &parts)) {
        return CUP_ERR_INVALID_RELEASE;
    }
    length = (size_t)(parts.base_end - release);
    if (length >= base_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(base, release, length);
    base[length] = '\0';
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
