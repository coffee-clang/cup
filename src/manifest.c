#include "manifest.h"

#include "filesystem.h"
#include "constants.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "util.h"
#include "entry.h"

#include <stdio.h>
#include <string.h>

// PATH / KEY / TEMPLATE HELPERS
static CupError resolve_manifest_path(char *buffer, size_t size) {
    CupError err;
    char user_manifest[MAX_PATH_LEN];
    int exists;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_manifest_file_path(user_manifest, sizeof(user_manifest));
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

static CupError split_manifest_key(const char *key, char *component, size_t component_size, char *tool, size_t tool_size, char *host_platform, size_t host_size, char *target_platform, size_t target_size, char *field, size_t field_size) {
    CupError err;
    char key_copy[MAX_MANIFEST_KEY_LEN];
    SplitOutput outputs[5];

    if (is_empty_string(key) || component == NULL || component_size == 0 || tool == NULL || tool_size == 0 ||
        host_platform == NULL || host_size == 0 || target_platform == NULL || target_size == 0 || field == NULL || field_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(key_copy, sizeof(key_copy), "%s", key);
    if (err != CUP_OK) {
        return err;
    }

    outputs[0].buffer = component;
    outputs[0].size = component_size;
    outputs[1].buffer = tool;
    outputs[1].size = tool_size;
    outputs[2].buffer = host_platform;
    outputs[2].size = host_size;
    outputs[3].buffer = target_platform;
    outputs[3].size = target_size;
    outputs[4].buffer = field;
    outputs[4].size = field_size;

    err = split_exact(key_copy, '.', outputs, 5);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

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

static int is_known_url_placeholder(const char *placeholder, size_t len) {
    static const char *known[] = {
        "{tool}",
        "{host_platform}",
        "{target_platform}",
        "{version}",
        "{format}"
    };
    size_t i;

    if (placeholder == NULL || len == 0) {
        return 0;
    }

    for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strlen(known[i]) == len && strncmp(placeholder, known[i], len) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError validate_url_template_placeholders(const char *url_template) {
    const char *cursor;

    if (is_empty_string(url_template)) {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = url_template;

    while ((cursor = strchr(cursor, '{')) != NULL) {
        const char *end = strchr(cursor, '}');

        if (end == NULL) {
            fprintf(stderr, "Error: manifest url_template contains an unterminated placeholder.\n");
            return CUP_ERR_MANIFEST;
        }

        if (!is_known_url_placeholder(cursor, (size_t)(end - cursor + 1))) {
            fprintf(stderr, "Error: manifest url_template contains unsupported placeholder '%.*s'.\n",
                (int)(end - cursor + 1), cursor);
            return CUP_ERR_MANIFEST;
        }

        cursor = end + 1;
    }

    return CUP_OK;
}

static CupError reject_unresolved_url_placeholders(const char *url) {
    if (is_empty_string(url)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strchr(url, '{') != NULL || strchr(url, '}') != NULL) {
        fprintf(stderr, "Error: resolved manifest URL still contains unresolved placeholders.\n");
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

// STORAGE HELPERS
static int manifest_has_key(const Manifest *manifest, const char *key) {
    size_t i;

    if (manifest == NULL || is_empty_string(key)) {
        return 0;
    }

    for (i = 0; i < manifest->count; ++i) {
        if (strcmp(manifest->fields[i].key, key) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError add_manifest_field(Manifest *manifest, const char *key, const char *value) {
    CupError err;

    if (manifest == NULL || is_empty_string(key) || is_empty_string(value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (manifest_has_key(manifest, key)) {
        fprintf(stderr, "Error: duplicate manifest key '%s'.\n", key);
        return CUP_ERR_MANIFEST;
    }

    if (manifest->count >= MAX_MANIFEST_FIELDS) {
        fprintf(stderr, "Error: manifest contains too many fields.\n");
        return CUP_ERR_MANIFEST;
    }

    err = checked_snprintf(manifest->fields[manifest->count].key, sizeof(manifest->fields[manifest->count].key), "%s", key);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = checked_snprintf(manifest->fields[manifest->count].value, sizeof(manifest->fields[manifest->count].value), "%s", value);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    manifest->count++;
    return CUP_OK;
}

const char *get_manifest_value(const Manifest *manifest, const char *key) {
    size_t i;

    if (manifest == NULL || is_empty_string(key)) {
        return NULL;
    }

    for (i = 0; i < manifest->count; ++i) {
        if (strcmp(manifest->fields[i].key, key) == 0) {
            return manifest->fields[i].value;
        }
    }

    return NULL;
}

// READ / QUERY HELPERS
static CupError read_manifest_value(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *field) {
    CupError err;
    char key[MAX_MANIFEST_KEY_LEN];
    const char *value;

    if (manifest == NULL || buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(field)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_manifest_key(key, sizeof(key), component, tool, host_platform, target_platform, field);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    value = get_manifest_value(manifest, key);
    if (value == NULL) {
        fprintf(stderr, "Error: tool '%s' for component '%s' is not configured for host '%s', target '%s' (missing field '%s').\n",
                tool, component, host_platform, target_platform, field);
        return CUP_ERR_MANIFEST;
    }

    err = checked_snprintf(buffer, size, "%s", value);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

static CupError manifest_list_contains(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *field, const char *expected, int *contains) {
    CupError err;
    char value[MAX_MANIFEST_VALUE_LEN];

    if (manifest == NULL || is_empty_string(component) || is_empty_string(tool) || is_empty_string(host_platform) || 
        is_empty_string(target_platform) || is_empty_string(field) || is_empty_string(expected) || contains == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(manifest, value, sizeof(value), component, tool, host_platform, target_platform, field);
    if (err != CUP_OK) {
        return err;
    }

    err = split_list_contains(value, ',', expected, contains);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: manifest list '%s' contains an invalid value.\n", field);
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

// VALIDATION HELPERS
static int is_manifest_field_name(const char *field) {
    if (is_empty_string(field)) {
        return 0;
    }

    return strcmp(field, "stable_version") == 0 || strcmp(field, "available_versions") == 0 ||
           strcmp(field, "default_format") == 0 || strcmp(field, "formats") == 0 || strcmp(field, "url_template") == 0;
}

static CupError validate_manifest_line(const char *key) {
    CupError err;
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char field[MAX_NAME_LEN];

    if (is_empty_string(key)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_manifest_key(key, component, sizeof(component), tool, sizeof(tool), host_platform, 
        sizeof(host_platform), target_platform, sizeof(target_platform), field, sizeof(field));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: malformed manifest key '%s'. Expected '<component>.<tool>.<host>.<target>.<field>'.\n", key);
        return CUP_ERR_MANIFEST;
    }

    err = validate_component(component);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_tool_for_component(component, tool);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_platform(host_platform);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_platform(target_platform);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (!is_manifest_field_name(field)) {
        fprintf(stderr, "Error: unknown manifest field '%s'.\n", field);
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

static CupError validate_manifest_config(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;
    char stable_version[MAX_MANIFEST_VALUE_LEN];
    char default_format[MAX_MANIFEST_VALUE_LEN];
    char url_template[MAX_MANIFEST_URL_LEN];
    int contains;

    if (manifest == NULL || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(manifest, stable_version, sizeof(stable_version), component, tool, host_platform, 
        target_platform, "stable_version");
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = manifest_list_contains(manifest, component, tool, host_platform, target_platform, "available_versions", 
        stable_version, &contains);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (!contains) {
        fprintf(stderr, "Error: manifest stable_version '%s' is not listed in available_versions for '%s.%s.%s.%s'.\n",
            stable_version, component, tool, host_platform, target_platform);
        return CUP_ERR_MANIFEST;
    }

    err = read_manifest_value(manifest, default_format, sizeof(default_format), component, tool, host_platform, 
        target_platform, "default_format");
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = manifest_list_contains(manifest, component, tool, host_platform, target_platform, "formats", default_format, &contains);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (!contains) {
        fprintf(stderr, "Error: manifest default_format '%s' is not listed in formats for '%s.%s.%s.%s'.\n",
            default_format, component, tool, host_platform, target_platform);
        return CUP_ERR_MANIFEST;
    }

    err = read_manifest_value(manifest, url_template, sizeof(url_template), component, tool, host_platform, 
        target_platform, "url_template");
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_url_template_placeholders(url_template);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

static int manifest_config_seen_before(const Manifest *manifest, size_t current_index, const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;
    char seen_component[MAX_NAME_LEN];
    char seen_tool[MAX_NAME_LEN];
    char seen_host[MAX_PLATFORM_LEN];
    char seen_target[MAX_PLATFORM_LEN];
    char seen_field[MAX_NAME_LEN];
    size_t i;

    if (manifest == NULL || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return 0;
    }

    i = current_index;

    while (i > 0) {
        i--;

        err = split_manifest_key(manifest->fields[i].key, seen_component, sizeof(seen_component), seen_tool, sizeof(seen_tool),
            seen_host, sizeof(seen_host), seen_target, sizeof(seen_target), seen_field, sizeof(seen_field));
        if (err != CUP_OK) {
            return 0;
        }

        if (strcmp(seen_component, component) == 0 && strcmp(seen_tool, tool) == 0 &&
            strcmp(seen_host, host_platform) == 0 && strcmp(seen_target, target_platform) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError validate_manifest(const Manifest *manifest) {
    CupError err;
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char field[MAX_NAME_LEN];
    size_t i;

    if (manifest == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < manifest->count; ++i) {
        err = split_manifest_key(manifest->fields[i].key, component, sizeof(component), tool, sizeof(tool), host_platform,
            sizeof(host_platform), target_platform, sizeof(target_platform), field, sizeof(field));
        if (err != CUP_OK) {
            return CUP_ERR_MANIFEST;
        }

        if (manifest_config_seen_before(manifest, i, component, tool, host_platform, target_platform)) {
            continue;
        }

        err = validate_manifest_config(manifest, component, tool, host_platform, target_platform);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

// PARSING / LOADING
static CupError parse_manifest_line(Manifest *manifest, char *line) {
    CupError err;
    char key[MAX_MANIFEST_KEY_LEN];
    char value[MAX_MANIFEST_VALUE_LEN];

    if (manifest == NULL || line == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = split_key_value(line, key, sizeof(key), value, sizeof(value));
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_manifest_line(key);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    err = add_manifest_field(manifest, key, value);
    if (err != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

CupError manifest_load(Manifest *manifest) {
    CupError err;
    FILE *file;
    char manifest_path[MAX_PATH_LEN];
    char line[MAX_MANIFEST_LINE_LEN];
    size_t line_number = 0;

    if (manifest == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(manifest, 0, sizeof(*manifest));

    err = resolve_manifest_path(manifest_path, sizeof(manifest_path));
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(manifest_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: could not read package manifest '%s'.\n", manifest_path);
        return CUP_ERR_MANIFEST;
    }

    line_number = 0;

    while (1) {
        int has_line;

        err = read_text_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            if (err == CUP_ERR_BUFFER_TOO_SMALL) {
                fprintf(stderr, "Error: manifest line %zu is too long.\n", line_number);
            } else {
                fprintf(stderr, "Error: could not read manifest line.\n");
            }

            fclose(file);
            return CUP_ERR_MANIFEST;
        }

        if (!has_line) {
            break;
        }

        err = parse_manifest_line(manifest, line);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid manifest line %zu.\n", line_number);
            fclose(file);
            return CUP_ERR_MANIFEST;
        }
    }

    if (fclose(file) != 0) {
        return CUP_ERR_MANIFEST;
    }

    err = validate_manifest(manifest);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// PUBLIC SEMANTIC API
CupError resolve_stable_release(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;

    if (manifest == NULL || buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(manifest, buffer, size, component, tool, host_platform, target_platform, "stable_version");
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError is_stable_version(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_stable) {
    CupError err;
    char stable_version[MAX_NAME_LEN];

    if (manifest == NULL || is_empty_string(component) || is_empty_string(tool) || is_empty_string(host_platform) ||
        is_empty_string(target_platform) || is_empty_string(version) || is_stable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_stable = 0;

    err = resolve_stable_release(manifest, stable_version, sizeof(stable_version), component, tool, host_platform, target_platform);
    if (err != CUP_OK) {
        return err;
    }

    *is_stable = strcmp(stable_version, version) == 0;
    return CUP_OK;
}

CupError is_version_available(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_available) {
    return manifest_list_contains(manifest, component, tool, host_platform, target_platform, "available_versions", version, is_available);
}

CupError get_default_format(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;

    if (manifest == NULL || buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(manifest, buffer, size, component, tool, host_platform, target_platform, "default_format");
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError is_format_supported(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *format, int *is_supported) {
    return manifest_list_contains(manifest, component, tool, host_platform, target_platform, "formats", format, is_supported);
}

CupError build_download_url(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, const char *format) {
    CupError err;
    char url_template[MAX_MANIFEST_URL_LEN];
    char step1[MAX_MANIFEST_URL_LEN];
    char step2[MAX_MANIFEST_URL_LEN];
    char step3[MAX_MANIFEST_URL_LEN];
    char step4[MAX_MANIFEST_URL_LEN];

    if (manifest == NULL || buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version) || is_empty_string(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_manifest_value(manifest, url_template, sizeof(url_template), component, tool, host_platform, target_platform, "url_template");
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step1, sizeof(step1), url_template, "{tool}", tool);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step2, sizeof(step2), step1, "{host_platform}", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step3, sizeof(step3), step2, "{target_platform}", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(step4, sizeof(step4), step3, "{version}", version);
    if (err != CUP_OK) {
        return err;
    }

    err = replace_placeholder(buffer, size, step4, "{format}", format);
    if (err != CUP_OK) {
        return err;
    }

    err = reject_unresolved_url_placeholders(buffer);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}
