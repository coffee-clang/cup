#include "manifest.h"
#include "constants.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static char *trim_spaces(char *text) {
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (*text == ' ' || *text == '\t') {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    return text;
}

static CupError replace_placeholder(char *buffer, size_t size, const char *template_str, const char *placeholder, const char *value) {
    CupError err;
    const char *cursor;
    const char *match;
    char temp[MAX_MANIFEST_URL_LEN];
    size_t placeholder_len;
    size_t value_len;
    size_t written;

    if (buffer == NULL || template_str == NULL || placeholder == NULL || value == NULL ||
        size == 0 || placeholder[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = template_str;
    placeholder_len = strlen(placeholder);
    value_len = strlen(value);
    written = 0;
    temp[0] = '\0';

    while ((match = strstr(cursor, placeholder)) != NULL) {
        size_t prefix_len = (size_t)(match - cursor);

        if (written + prefix_len + value_len + 1 > sizeof(temp)) {
            fprintf(stderr, "Error: resolved manifest value is too long.\n");
            return CUP_ERR_FETCH;
        }

        memcpy(temp + written, cursor, prefix_len);
        written += prefix_len;

        memcpy(temp + written, value, value_len);
        written += value_len;

        cursor = match + placeholder_len;
    }

    if (written + strlen(cursor) + 1 > sizeof(temp)) {
        fprintf(stderr, "Error: resolved manifest is too long.\n");
        return CUP_ERR_FETCH;
    }

    strcpy(temp + written, cursor);

    err = checked_snprintf(buffer, size, "%s", temp);
    return err;
}

static CupError get_manifest_path(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "config/packages.cfg");
}

static CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *key_suffix) {
    CupError err;
    FILE *file;
    char manifest_path[MAX_PATH_LEN];
    char line[MAX_MANIFEST_LINE_LEN];
    char key[MAX_MANIFEST_KEY_LEN];
    size_t key_len;

    if (buffer == NULL || component == NULL || tool == NULL || key_suffix == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || key_suffix[0] == '\0') {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_manifest_path(manifest_path, sizeof(manifest_path));
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(manifest_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open package manifest '%s'.\n", manifest_path);
        return CUP_ERR_FETCH;
    }

    err = checked_snprintf(key, sizeof(key), "%s.%s.%s=", component, tool, key_suffix);
    if (err != CUP_OK) {
        fclose(file);
        return CUP_ERR_FETCH;
    }

    key_len = strlen(key);

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len;

        if (strncmp(line, key, key_len) != 0) {
            continue;
        }

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        err = checked_snprintf(buffer, size, "%s", line + key_len);
        fclose(file);
        return err;
    }

    fclose(file);
    fprintf(stderr, "Error: no package source configured for %s.%s.%s.\n", component, tool, key_suffix);
    return CUP_ERR_FETCH;
}

static CupError manifest_list_contains(const char *component, const char *tool, const char *key_suffix, const char *needle, int *contains) {
    CupError err;
    char value[MAX_MANIFEST_VALUE_LEN];
    char *token;

    if (component == NULL || tool == NULL || key_suffix == NULL ||  needle == NULL || contains == NULL ||
        component[0] == '\0' || tool[0] == '\0' || key_suffix[0] == '\0' || needle[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    *contains = 0;

    err = read_manifest_value(value, sizeof(value), component, tool, key_suffix);
    if (err != CUP_OK) {
        return err;
    }

    token = strtok(value, ",");
    while (token != NULL) {
        token = trim_spaces(token);

        if (strcmp(token, needle) == 0) {
            *contains = 1;
            return CUP_OK;
        }

        token = strtok(NULL, ",");
    }

    return CUP_OK;
}

CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL || 
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0') {
        fprintf(stderr, "Error: invalid release resolution arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(release, "stable") == 0) {
        err = read_manifest_value(buffer, size, component, tool, "stable_version");
        if (err != CUP_OK) {
            return err;
        }

        return CUP_OK;
    }

    err = checked_snprintf(buffer, size, "%s", release);
    return err;
}

CupError is_stable_release(const char *component, const char *tool, const char *release, int *is_stable) {
    CupError err;
    char stable_release[MAX_NAME_LEN];

    if (component == NULL || tool == NULL || release == NULL || is_stable == NULL ||
        component[0] == '\0' || tool[0] == '\0' || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_stable = 0;

    err = resolve_release(stable_release, sizeof(stable_release), component, tool, "stable");
    if (err != CUP_OK) {
        return err;
    }

    if (strcmp(stable_release, release) == 0) {
        *is_stable = 1;
    }

    return CUP_OK;
}

CupError is_version_available(const char *component, const char *tool, const char *version, int *is_available) {
    return manifest_list_contains(component, tool, "available_versions", version, is_available);
}

CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool) {
    return read_manifest_value(buffer, size, component, tool, "default_format");
}

CupError is_format_supported(const char *component, const char *tool, const char *format, int *is_supported) {
    return manifest_list_contains(component, tool, "formats", format, is_supported);
}

CupError build_package_url_from_manifest(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *format) {
    CupError err;
    char url_template[MAX_MANIFEST_URL_LEN];
    char tmp[MAX_MANIFEST_URL_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL || format == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0' || format[0] == '\0') {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(url_template, sizeof(url_template), component, tool, "url_template");
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(tmp, sizeof(tmp), url_template, "{version}", release);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(buffer, size, tmp, "{format}", format);
    return err;
}