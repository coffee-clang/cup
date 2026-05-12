#include "manifest.h"
#include "constants.h"
#include "util.h"
#include "system.h"

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

    if (buffer == NULL || size == 0 || is_empty_string(template_str) || is_empty_string(placeholder) || is_empty_string(value)) {
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
    CupError err;
    char home[MAX_PATH_LEN];
    char user_manifest[MAX_PATH_LEN];
    int exists;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists("config/packages.cfg", &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists) {
        err = checked_snprintf(buffer, size, "%s", "config/packages.cfg");
        return err;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: package manifest not found at './config/packages.cfg' and home directory could not be resolved.\n");
        return err;
    }

    err = checked_snprintf(user_manifest, sizeof(user_manifest), "%s/.cup/config/packages.cfg", home);
    if (err != CUP_OK) {
        return err;
    }

    err = system_path_exists(user_manifest, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists) {
        err = checked_snprintf(buffer, size, "%s", user_manifest);
        return err;
    }

    fprintf(stderr,
        "Error: package manifest not found.\n"
        "Expected one of:\n"
        "  ./config/packages.cfg\n"
        "  %s\n\n"
        "Install cup with:\n"
        "  curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh\n",
        user_manifest
    );

    return CUP_ERR_FETCH;
}

static CupError build_manifest_key(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *field) {
    CupError err;
    
    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(field)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s.%s.%s.%s.%s", component, tool, host_platform, target_platform, field);
    return err;
}

static CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *field) {
    CupError err;
    FILE *file;
    char manifest_path[MAX_PATH_LEN];
    char line[MAX_MANIFEST_LINE_LEN];
    char key[MAX_MANIFEST_KEY_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(field)) {
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

    err = build_manifest_key(key, sizeof(key), component, tool, host_platform, target_platform, field);
    if (err != CUP_OK) {
        fclose(file);
        return CUP_ERR_FETCH;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *line_key;
        char *line_value;
        size_t len;

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        line_key = trim_spaces(line);

        if (line_key[0] == '\0' || line_key[0] == '#') {
            continue;
        }

        equals = strchr(line_key, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';

        line_key = trim_spaces(line_key);
        line_value = trim_spaces(equals + 1);

        if (strcmp(line_key, key) == 0) {
            err = checked_snprintf(buffer, size, "%s", line_value);
            fclose(file);
            return err;
        }
    }

    fclose(file);
    fprintf(stderr, "Error: tool '%s' for component '%s' is not configured for host '%s', target '%s' (missing field '%s').\n", 
            tool, component, host_platform, target_platform, field);
    return CUP_ERR_FETCH;
}

static CupError manifest_list_contains(const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *field, const char *needle, int *contains) {
    CupError err;
    char value[MAX_MANIFEST_VALUE_LEN];
    char *token;

    if (contains == NULL || is_empty_string(component) || is_empty_string(tool) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(field) || is_empty_string(needle)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *contains = 0;

    err = read_manifest_value(value, sizeof(value), component, tool, host_platform, target_platform, field);
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

CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *release) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(release)) {
        fprintf(stderr, "Error: invalid release resolution arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(release, "stable") == 0) {
        err = read_manifest_value(buffer, size, component, tool, host_platform, target_platform, "stable_version");
        if (err != CUP_OK) {
            return err;
        }

        return CUP_OK;
    }

    err = checked_snprintf(buffer, size, "%s", release);
    return err;
}

CupError is_stable_version(const char *component, const char *tool, const char *host_platform, const char *target_platform, 
    const char *version, int *is_stable) {
    CupError err;
    char stable_version[MAX_NAME_LEN];

    if (is_stable == NULL || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_stable = 0;

    err = resolve_release(stable_version, sizeof(stable_version), component, tool, host_platform, target_platform, "stable");
    if (err != CUP_OK) {
        return err;
    }

    if (strcmp(stable_version, version) == 0) {
        *is_stable = 1;
    }

    return CUP_OK;
}

CupError is_version_available(const char *component, const char *tool, const char *host_platform, const char *target_platform, 
    const char *version, int *is_available) {
    return manifest_list_contains(component, tool, host_platform, target_platform, "available_versions", version, is_available);
}

CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform) {
    return read_manifest_value(buffer, size, component, tool, host_platform, target_platform, "default_format");
}

CupError is_format_supported(const char *component, const char *tool, const char *host_platform, const char *target_platform, 
    const char *format, int *is_supported) {
    return manifest_list_contains(component, tool, host_platform, target_platform, "formats", format, is_supported);
}

CupError build_download_url(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *version, const char *format) {
    CupError err;
    char url_template[MAX_MANIFEST_URL_LEN];
    char step1[MAX_MANIFEST_URL_LEN];
    char step2[MAX_MANIFEST_URL_LEN];
    char step3[MAX_MANIFEST_URL_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version) || is_empty_string(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(url_template, sizeof(url_template), component, tool, host_platform, target_platform, "url_template");
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step1, sizeof(step1), url_template, "{host_platform}", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step2, sizeof(step2), step1, "{target_platform}", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step3, sizeof(step3), step2, "{version}", version);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(buffer, size, step3, "{format}", format);
    return err;
}