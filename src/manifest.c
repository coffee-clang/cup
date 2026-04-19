#include <stdio.h>
#include <string.h>

#include "manifest.h"
#include "fs.h"

static CupError replace_placeholder(char *buffer, size_t size, const char *template_str, const char *placeholder, const char *value) {
    const char *cursor;
    const char *match;
    char temp[MAX_PATH_LEN];
    size_t placeholder_len;
    size_t value_len;
    size_t written;

    if (buffer == NULL || template_str == NULL || placeholder == NULL || value == NULL) {
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

    return checked_snprintf(buffer, size, "%s", temp);
}

CupError get_package_manifest_path(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    return checked_snprintf(buffer, size, "config/packages.cfg");
}

CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *key_suffix) {
    CupError err;
    FILE *file;
    char manifest_path[MAX_PATH_LEN];
    char line[1024];
    char key[MAX_NAME_LEN *3];
    size_t key_len;

    if (buffer == NULL || component == NULL || tool == NULL || key_suffix == NULL) {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_package_manifest_path(manifest_path, sizeof(manifest_path));
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

        fclose(file);
        return checked_snprintf(buffer, size, "%s", line + key_len);
    }

    fclose(file);
    fprintf(stderr, "Error: no package source configured for %s.%s.%s.\n", component, tool, key_suffix);
    return CUP_ERR_FETCH;
}

CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    if (buffer == NULL || component == NULL || tool == NULL || release == NULL || size == 0) {
        fprintf(stderr, "Error: invalid release resolution arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(release, "stable") == 0) {
        return read_manifest_value(buffer, size, component, tool, "stable_version");
    }

    return checked_snprintf(buffer, size, "%s", release);
}

CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool) {
    return read_manifest_value(buffer, size, component, tool, "default_format");
}

CupError is_format_supported(const char *component, const char *tool, const char *format, int *is_supported) {
    CupError err;
    char formats[256];
    char *token;

    if (format == NULL || is_supported == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_supported = 0;

    err = read_manifest_value(formats, sizeof(formats), component, tool, "formats");
    if (err != CUP_OK) {
        return err;
    }

    token = strtok(formats, ",");
    while (token != NULL) {
        if (strcmp(token, format) == 0) {
            *is_supported = 1;
            return CUP_OK;
        }

        token = strtok(NULL, ",");
    }

    return CUP_OK;
}

CupError build_package_url_from_manifest(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *format) {
    CupError err;
    char url_template[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL || format == NULL) {
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

    return replace_placeholder(buffer, size, tmp, "{format}", format);
}