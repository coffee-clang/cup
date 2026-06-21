#include "manifest.h"

#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_STABLE_VERSION     (1u << 0)
#define FIELD_AVAILABLE_VERSIONS (1u << 1)
#define FIELD_DEFAULT_FORMAT     (1u << 2)
#define FIELD_FORMATS            (1u << 3)
#define FIELD_URL_TEMPLATE       (1u << 4)
#define REQUIRED_FIELDS (FIELD_STABLE_VERSION | FIELD_AVAILABLE_VERSIONS | \
    FIELD_DEFAULT_FORMAT | FIELD_FORMATS | FIELD_URL_TEMPLATE)
#define DEVELOPMENT_MANIFEST_PATH "config/packages.cfg"

// MANIFEST LIFECYCLE
void manifest_init(Manifest *manifest) {
    if (manifest != NULL) {
        memset(manifest, 0, sizeof(*manifest));
    }
}

void manifest_free(Manifest *manifest) {
    if (manifest == NULL) {
        return;
    }

    free(manifest->packages);
    manifest_init(manifest);
}

// PACKAGE STORAGE
static int find_package_index(const Manifest *manifest, const char *component,
    const char *tool, const char *host, const char *target) {
    size_t i;

    if (manifest == NULL) {
        return -1;
    }

    for (i = 0; i < manifest->count; ++i) {
        const ManifestPackage *package = &manifest->packages[i];

        if (strcmp(package->component, component) == 0 &&
            strcmp(package->tool, tool) == 0 &&
            strcmp(package->host_platform, host) == 0 &&
            strcmp(package->target_platform, target) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static CupError append_package(Manifest *manifest, const char *component,
    const char *tool, const char *host, const char *target,
    ManifestPackage **result) {
    ManifestPackage *packages;
    size_t capacity;

    if (manifest->count == manifest->capacity) {
        capacity = manifest->capacity == 0 ? 16 : manifest->capacity * 2;
        packages = realloc(manifest->packages, capacity * sizeof(*packages));
        if (packages == NULL) {
            return CUP_ERR_MANIFEST;
        }

        manifest->packages = packages;
        manifest->capacity = capacity;
    }

    *result = &manifest->packages[manifest->count++];
    memset(*result, 0, sizeof(**result));

    if (text_format((*result)->component,
        sizeof((*result)->component), "%s", component) != CUP_OK ||
        text_format((*result)->tool,
            sizeof((*result)->tool), "%s", tool) != CUP_OK ||
        text_format((*result)->host_platform,
            sizeof((*result)->host_platform), "%s", host) != CUP_OK ||
        text_format((*result)->target_platform,
            sizeof((*result)->target_platform), "%s", target) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}

// PARSING
static CupError parse_package_key(const char *key, char *component, size_t component_size,
    char *tool, size_t tool_size, char *host, size_t host_size,
    char *target, size_t target_size, char *field, size_t field_size) {
    char copy[MAX_MANIFEST_KEY_LEN];
    TextBuffer outputs[5];

    if (text_format(copy, sizeof(copy), "%s", key) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    outputs[0] = (TextBuffer){.data = component, .capacity = component_size};
    outputs[1] = (TextBuffer){.data = tool, .capacity = tool_size};
    outputs[2] = (TextBuffer){.data = host, .capacity = host_size};
    outputs[3] = (TextBuffer){.data = target, .capacity = target_size};
    outputs[4] = (TextBuffer){.data = field, .capacity = field_size};

    return text_split_exact(copy, '.', outputs, 5) == CUP_OK ?
        CUP_OK : CUP_ERR_MANIFEST;
}

static CupError set_package_field(ManifestPackage *package, const char *field, const char *value) {
    unsigned bit;
    char *destination;
    size_t destination_size;

    if (strcmp(field, "stable_version") == 0) {
        bit = FIELD_STABLE_VERSION;
        destination = package->stable_version;
        destination_size = sizeof(package->stable_version);
    } else if (strcmp(field, "available_versions") == 0) {
        bit = FIELD_AVAILABLE_VERSIONS;
        destination = package->available_versions;
        destination_size = sizeof(package->available_versions);
    } else if (strcmp(field, "default_format") == 0) {
        bit = FIELD_DEFAULT_FORMAT;
        destination = package->default_format;
        destination_size = sizeof(package->default_format);
    } else if (strcmp(field, "formats") == 0) {
        bit = FIELD_FORMATS;
        destination = package->formats;
        destination_size = sizeof(package->formats);
    } else if (strcmp(field, "url_template") == 0) {
        bit = FIELD_URL_TEMPLATE;
        destination = package->url_template;
        destination_size = sizeof(package->url_template);
    } else {
        fprintf(stderr, "Error: unknown manifest field '%s'.\n", field);
        return CUP_ERR_MANIFEST;
    }

    if ((package->field_mask & bit) != 0) {
        fprintf(stderr, "Error: duplicate manifest field '%s'.\n", field);
        return CUP_ERR_MANIFEST;
    }

    if (text_format(destination, destination_size, "%s", value) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    package->field_mask |= bit;
    return CUP_OK;
}

static CupError parse_manifest_line(Manifest *manifest, char *line) {
    char key[MAX_MANIFEST_KEY_LEN];
    char value[MAX_MANIFEST_URL_LEN];
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host[MAX_PLATFORM_LEN];
    char target[MAX_PLATFORM_LEN];
    char field[MAX_NAME_LEN];
    ManifestPackage *package;
    int index;

    if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK ||
        parse_package_key(key, component, sizeof(component), tool, sizeof(tool),
            host, sizeof(host), target, sizeof(target), field, sizeof(field)) != CUP_OK ||
        registry_validate_component(component) != CUP_OK ||
        registry_validate_tool(component, tool) != CUP_OK ||
        platform_validate(host) != CUP_OK || platform_validate(target) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    index = find_package_index(manifest, component, tool, host, target);
    if (index == -1) {
        if (append_package(manifest, component, tool, host, target, &package) != CUP_OK) {
            return CUP_ERR_MANIFEST;
        }
    } else {
        package = &manifest->packages[index];
    }

    return set_package_field(package, field, value);
}

// VALIDATION
static CupError validate_value_list(const char *value, const char *expected, int *contains) {
    char copy[MAX_MANIFEST_VALUE_LEN];
    char *cursor;

    if (contains != NULL) {
        *contains = 0;
    }

    if (text_format(copy, sizeof(copy), "%s", value) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    cursor = copy;

    while (cursor != NULL) {
        char *separator = strchr(cursor, ',');
        char *part;

        if (separator != NULL) {
            *separator = '\0';
        }

        part = text_trim(cursor);
        if (!path_is_safe_identifier(part)) {
            return CUP_ERR_MANIFEST;
        }

        if (contains != NULL && expected != NULL && strcmp(part, expected) == 0) {
            *contains = 1;
        }

        cursor = separator == NULL ? NULL : separator + 1;
    }

    return CUP_OK;
}

static int is_known_placeholder(const char *start, size_t length) {
    static const char *const placeholders[] = {
        "{tool}",
        "{host_platform}",
        "{target_platform}",
        "{version}",
        "{format}"
    };
    size_t i;

    for (i = 0; i < sizeof(placeholders) / sizeof(placeholders[0]); ++i) {
        if (strlen(placeholders[i]) == length &&
            strncmp(start, placeholders[i], length) == 0) {
            return 1;
        }
    }

    return 0;
}

static CupError validate_url_template(const char *url) {
    const char *cursor;

    if (strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "Error: manifest URL templates must use HTTPS.\n");
        return CUP_ERR_MANIFEST;
    }

    cursor = url;

    while (*cursor != '\0') {
        const char *end;

        if (*cursor == '}') {
            return CUP_ERR_MANIFEST;
        }

        if (*cursor != '{') {
            cursor++;
            continue;
        }

        end = strchr(cursor, '}');
        if (end == NULL || !is_known_placeholder(cursor, (size_t)(end - cursor + 1))) {
            return CUP_ERR_MANIFEST;
        }

        cursor = end + 1;
    }

    return CUP_OK;
}

static CupError validate_manifest_package(const ManifestPackage *package) {
    int contains;

    if (package->field_mask != REQUIRED_FIELDS ||
        !path_is_safe_identifier(package->stable_version) ||
        !path_is_safe_identifier(package->default_format)) {
        return CUP_ERR_MANIFEST;
    }

    if (validate_value_list(package->available_versions,
        package->stable_version, &contains) != CUP_OK || !contains) {
        fprintf(stderr, "Error: stable_version is not listed in "
            "available_versions for '%s.%s.%s.%s'.\n",
            package->component, package->tool,
            package->host_platform, package->target_platform);
        return CUP_ERR_MANIFEST;
    }

    if (validate_value_list(package->formats,
        package->default_format, &contains) != CUP_OK || !contains) {
        fprintf(stderr, "Error: default_format is not listed in formats for '%s.%s.%s.%s'.\n",
            package->component, package->tool,
            package->host_platform, package->target_platform);
        return CUP_ERR_MANIFEST;
    }

    return validate_url_template(package->url_template);
}

// LOADING
CupError manifest_load_path(Manifest *manifest, const char *path, ManifestSource source) {
    FILE *file;
    CupError err;
    char line[MAX_MANIFEST_LINE_LEN];
    size_t line_number = 0;
    size_t i;

    if (manifest == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    manifest_free(manifest);
    manifest->source = source;

    if (text_format(manifest->path, sizeof(manifest->path), "%s", path) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return CUP_ERR_MANIFEST;
    }

    while (1) {
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid package manifest line %zu.\n", line_number);
            fclose(file);
            manifest_free(manifest);
            return CUP_ERR_MANIFEST;
        }

        if (!has_line) {
            break;
        }

        if (parse_manifest_line(manifest, line) != CUP_OK) {
            fprintf(stderr, "Error: invalid package manifest line %zu.\n", line_number);
            fclose(file);
            manifest_free(manifest);
            return CUP_ERR_MANIFEST;
        }
    }

    if (fclose(file) != 0 || manifest->count == 0) {
        manifest_free(manifest);
        return CUP_ERR_MANIFEST;
    }

    for (i = 0; i < manifest->count; ++i) {
        if (validate_manifest_package(&manifest->packages[i]) != CUP_OK) {
            manifest_free(manifest);
            return CUP_ERR_MANIFEST;
        }
    }

    return CUP_OK;
}

CupError manifest_load_installed(Manifest *manifest) {
    char path[MAX_PATH_LEN];

    if (layout_get_manifest_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    return manifest_load_path(manifest, path, MANIFEST_SOURCE_INSTALLED);
}

CupError manifest_load_development(Manifest *manifest) {
    return manifest_load_path(manifest, DEVELOPMENT_MANIFEST_PATH,
        MANIFEST_SOURCE_DEVELOPMENT);
}

CupError manifest_load(Manifest *manifest) {
    char installed[MAX_PATH_LEN];
    int exists;

    if (manifest == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (layout_get_manifest_path(installed, sizeof(installed)) != CUP_OK ||
        system_path_exists(installed, &exists) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (exists) {
        return manifest_load_path(manifest, installed, MANIFEST_SOURCE_INSTALLED);
    }

    if (system_path_exists(DEVELOPMENT_MANIFEST_PATH, &exists) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (exists) {
        return manifest_load_development(manifest);
    }

    fprintf(stderr, "Error: package manifest not found at '%s' or './%s'.\n",
        installed, DEVELOPMENT_MANIFEST_PATH);
    return CUP_ERR_MANIFEST;
}

// QUERIES
static const ManifestPackage *require_package(const Manifest *manifest,
    const char *component, const char *tool, const char *host, const char *target) {
    int index;

    index = find_package_index(manifest, component, tool, host, target);
    if (index == -1) {
        fprintf(stderr, "Error: tool '%s' for component '%s' is not "
            "configured for host '%s', target '%s'.\n",
            tool, component, host, target);
        return NULL;
    }

    return &manifest->packages[index];
}

CupError manifest_resolve_stable(const Manifest *manifest, char *buffer, size_t size,
    const char *component, const char *tool, const char *host, const char *target) {
    const ManifestPackage *package;

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    return text_format(buffer, size, "%s", package->stable_version);
}

CupError manifest_is_stable(const Manifest *manifest, const char *component,
    const char *tool, const char *host, const char *target,
    const char *version, int *is_stable) {
    const ManifestPackage *package;

    if (is_stable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    *is_stable = strcmp(package->stable_version, version) == 0;
    return CUP_OK;
}

CupError manifest_has_version(const Manifest *manifest, const char *component,
    const char *tool, const char *host, const char *target,
    const char *version, int *is_available) {
    const ManifestPackage *package;

    if (is_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    return validate_value_list(package->available_versions, version, is_available);
}

CupError manifest_get_default_format(const Manifest *manifest, char *buffer,
    size_t size, const char *component, const char *tool,
    const char *host, const char *target) {
    const ManifestPackage *package;

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    return text_format(buffer, size, "%s", package->default_format);
}

CupError manifest_has_format(const Manifest *manifest, const char *component,
    const char *tool, const char *host, const char *target,
    const char *format, int *is_supported) {
    const ManifestPackage *package;

    if (is_supported == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    return validate_value_list(package->formats, format, is_supported);
}

// URL EXPANSION
static CupError replace_placeholder(char *buffer, size_t size, const char *input,
    const char *placeholder, const char *value) {
    const char *cursor = input;
    const char *match;
    size_t written = 0;
    size_t placeholder_length = strlen(placeholder);
    size_t value_length = strlen(value);

    while ((match = strstr(cursor, placeholder)) != NULL) {
        size_t prefix_length = (size_t)(match - cursor);

        if (written + prefix_length + value_length + 1 > size) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }

        memcpy(buffer + written, cursor, prefix_length);
        written += prefix_length;
        memcpy(buffer + written, value, value_length);
        written += value_length;
        cursor = match + placeholder_length;
    }

    if (written + strlen(cursor) + 1 > size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    strcpy(buffer + written, cursor);
    return CUP_OK;
}

CupError manifest_build_url(const Manifest *manifest, char *buffer, size_t size,
    const char *component, const char *tool, const char *host,
    const char *target, const char *version, const char *format) {
    const ManifestPackage *package;
    char step1[MAX_MANIFEST_URL_LEN];
    char step2[MAX_MANIFEST_URL_LEN];
    char step3[MAX_MANIFEST_URL_LEN];
    char step4[MAX_MANIFEST_URL_LEN];

    package = require_package(manifest, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_MANIFEST;
    }

    if (replace_placeholder(step1, sizeof(step1), package->url_template,
        "{tool}", tool) != CUP_OK ||
        replace_placeholder(step2, sizeof(step2), step1,
            "{host_platform}", host) != CUP_OK ||
        replace_placeholder(step3, sizeof(step3), step2,
            "{target_platform}", target) != CUP_OK ||
        replace_placeholder(step4, sizeof(step4), step3,
            "{version}", version) != CUP_OK ||
        replace_placeholder(buffer, size, step4,
            "{format}", format) != CUP_OK) {
        return CUP_ERR_MANIFEST;
    }

    if (strchr(buffer, '{') != NULL || strchr(buffer, '}') != NULL ||
        strncmp(buffer, "https://", 8) != 0) {
        return CUP_ERR_MANIFEST;
    }

    return CUP_OK;
}
