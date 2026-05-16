#include "manifest.h"

#include "constants.h"
#include "path.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// TEMPLATE HELPERS
static CupError replace_placeholder(char *buffer, size_t size, const char *template_str, const char *placeholder, const char *value) {
    CupError err;
    const char *cursor;
    const char *match;
    char temp[MAX_MANIFEST_URL_LEN];
    size_t placeholder_len;
    size_t value_len;
    size_t written;

    if (buffer == NULL || size == 0 || is_empty_string(template_str) || 
        is_empty_string(placeholder) || is_empty_string(value)) {
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
            return CUP_ERR_MANIFEST;
        }

        memcpy(temp + written, cursor, prefix_len);
        written += prefix_len;

        memcpy(temp + written, value, value_len);
        written += value_len;

        cursor = match + placeholder_len;
    }

    if (written + strlen(cursor) + 1 > sizeof(temp)) {
        fprintf(stderr, "Error: resolved manifest is too long.\n");
        return CUP_ERR_MANIFEST;
    }

    strcpy(temp + written, cursor);

    err = checked_snprintf(buffer, size, "%s", temp);
    return err;
}

// MANIFEST PATH / KEYS
static CupError get_manifest_path(char *buffer, size_t size) {
    CupError err;
    char home[MAX_PATH_LEN];
    char cup_root[MAX_PATH_LEN];
    char config_root[MAX_PATH_LEN];
    char user_manifest[MAX_PATH_LEN];
    int exists;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: home directory could not be resolved while looking for package manifest.\n");
        return err;
    }

    err = path_join(cup_root, sizeof(cup_root), home, ".cup");
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(config_root, sizeof(config_root), cup_root, "config");
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(user_manifest, sizeof(user_manifest), config_root, "packages.cfg");
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

    err = system_path_exists("config/packages.cfg", &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists) {
        err = checked_snprintf(buffer, size, "%s", "config/packages.cfg");
        return err;
    }

    fprintf(stderr,
        "Error: package manifest not found.\n"
        "Expected one of:\n"
        "  %s\n"
        "  ./config/packages.cfg\n\n"
        "Install cup with:\n"
        "  curl -fsSL https://github.com/coffee-clang/cup/releases/download/cup-bootstrap/install.sh | sh\n",
        user_manifest
    );

    return CUP_ERR_MANIFEST;
}

static CupError build_manifest_key(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *field) {
    CupError err;
    
    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(field)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s.%s.%s.%s.%s", component, tool, host_platform, target_platform, field);
    return err;
}

// MANIFEST READ
static CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *field) {
    CupError err;
    char manifest_path[MAX_PATH_LEN];
    char key[MAX_MANIFEST_KEY_LEN];
    int found;

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(field)) {
        fprintf(stderr, "Error: invalid manifest lookup arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_manifest_path(manifest_path, sizeof(manifest_path));
    if (err != CUP_OK) {
        return err;
    }

    err = build_manifest_key(key, sizeof(key), component, tool, host_platform, target_platform, field);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = read_key_value_field(buffer, size, manifest_path, key, &found);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not read package manifest '%s'.\n", manifest_path);
        return CUP_ERR_MANIFEST;
    }

    if (!found) {
        fprintf(stderr, "Error: tool '%s' for component '%s' is not configured for host '%s', target '%s' (missing field '%s').\n", 
            tool, component, host_platform, target_platform, field);
        return CUP_ERR_MANIFEST;
    }

    if (is_empty_string(buffer)) {
        fprintf(stderr, "Error: manifest field '%s' for tool '%s' and component '%s' on host '%s', target '%s' is empty.\n",
            field, tool, component, host_platform, target_platform);
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

static CupError manifest_list_contains(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *field, const char *needle, int *contains) {
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

// RELEASE
CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *release) {
    CupError err;
    const char *resolved_release;
    int is_available;

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(release)) {
        fprintf(stderr, "Error: invalid release resolution arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    resolved_release = release;

    if (strcmp(release, "stable") == 0) {
        err = read_manifest_value(buffer, size, component, tool, host_platform, target_platform, "stable_version");
        if (err != CUP_OK) {
            return err;
        }

        resolved_release = buffer;
    }

    err = manifest_list_contains(component, tool, host_platform, target_platform, "available_versions", 
        resolved_release, &is_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!is_available) {
        fprintf(stderr, "Error: version '%s' is not listed in available versions for tool '%s' on host '%s', target '%s'.\n",
            resolved_release, tool, host_platform, target_platform);
        return CUP_ERR_MANIFEST;
    }

    if (strcmp(release, "stable") != 0) {
        err = checked_snprintf(buffer, size, "%s", release);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

CupError is_stable_version(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_stable) {
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

CupError is_version_available(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_available) {
    return manifest_list_contains(component, tool, host_platform, target_platform, "available_versions", version, is_available);
}

// FORMAT
CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;
    int is_supported;

    err = read_manifest_value(buffer, size, component, tool, host_platform, target_platform, "default_format");
    if (err != CUP_OK) {
        return err;
    }

    err = manifest_list_contains(component, tool, host_platform, target_platform, "formats", buffer, &is_supported);
    if (err != CUP_OK) {
        return err;
    }

    if (!is_supported) {
        fprintf(stderr,
            "Error: default archive format '%s' is not listed in supported formats for tool '%s' on host '%s', target '%s'.\n",
            buffer, tool, host_platform, target_platform);
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

CupError is_format_supported(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *format, int *is_supported) {
    return manifest_list_contains(component, tool, host_platform, target_platform, "formats", format, is_supported);
}

// URL
CupError build_download_url(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, const char *format) {
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