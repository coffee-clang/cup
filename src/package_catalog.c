/*
 * Loads and validates packages.cfg, groups fields into concrete host/target package tuples,
 * resolves stable versions and expands HTTPS templates.
 */

#include "package_catalog.h"

#include "layout.h"
#include "path.h"
#include "package_archive.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_STABLE_VERSION (1u << 0)
#define FIELD_AVAILABLE_VERSIONS (1u << 1)
#define FIELD_DEFAULT_FORMAT (1u << 2)
#define FIELD_FORMATS (1u << 3)
#define FIELD_URL_TEMPLATE (1u << 4)
#define FIELD_CHECKSUM_URL (1u << 5)
#define REQUIRED_FIELDS \
    (FIELD_STABLE_VERSION | FIELD_AVAILABLE_VERSIONS | FIELD_DEFAULT_FORMAT | FIELD_FORMATS | \
     FIELD_URL_TEMPLATE | FIELD_CHECKSUM_URL)
#define DEVELOPMENT_MANIFEST_PATH "config/packages.cfg"

/* Catalog lifetime and tuple assembly. A package becomes visible only after every required
 * field is present. */
void package_catalog_init(PackageCatalog *catalog) {
    if (catalog != NULL) {
        memset(catalog, 0, sizeof(*catalog));
    }
}

void package_catalog_free(PackageCatalog *catalog) {
    if (catalog == NULL) {
        return;
    }

    free(catalog->packages);
    package_catalog_init(catalog);
}

/* Bounded storage ownership and duplicate-key detection. */
static int find_package_index(const PackageCatalog *catalog,
                              const char *component,
                              const char *tool,
                              const char *host,
                              const char *target) {
    size_t i;

    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target)) {
        return -1;
    }

    for (i = 0; i < catalog->count; ++i) {
        const PackageCatalogEntry *package = &catalog->packages[i];

        if (strcmp(package->component, component) == 0 && strcmp(package->tool, tool) == 0 &&
            strcmp(package->host_platform, host) == 0 &&
            strcmp(package->target_platform, target) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static CupError append_package(PackageCatalog *catalog,
                               const char *component,
                               const char *tool,
                               const char *host,
                               const char *target,
                               PackageCatalogEntry **result) {
    PackageCatalogEntry candidate = {0};
    PackageCatalogEntry *packages;
    CupError err;
    size_t capacity;

    if (catalog->count == catalog->capacity) {
        if (catalog->capacity > SIZE_MAX / 2) {
            return CUP_ERR_TEMPORARY;
        }
        capacity = catalog->capacity == 0 ? 16 : catalog->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*packages)) {
            return CUP_ERR_TEMPORARY;
        }
        packages = realloc(catalog->packages, capacity * sizeof(*packages));
        if (packages == NULL) {
            return CUP_ERR_TEMPORARY;
        }

        catalog->packages = packages;
        catalog->capacity = capacity;
    }

    err = text_copy(candidate.component, sizeof(candidate.component), component);
    if (err == CUP_OK) {
        err = text_copy(candidate.tool, sizeof(candidate.tool), tool);
    }
    if (err == CUP_OK) {
        err = text_copy(candidate.host_platform, sizeof(candidate.host_platform), host);
    }
    if (err == CUP_OK) {
        err = text_copy(candidate.target_platform, sizeof(candidate.target_platform), target);
    }
    if (err != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    catalog->packages[catalog->count] = candidate;
    *result = &catalog->packages[catalog->count++];
    return CUP_OK;
}

/* Temporary parser state for one component/tool/host/target tuple. */
static CupError parse_package_key(const char *key,
                                  char *component,
                                  size_t component_size,
                                  char *tool,
                                  size_t tool_size,
                                  char *host,
                                  size_t host_size,
                                  char *target,
                                  size_t target_size,
                                  char *field,
                                  size_t field_size) {
    char copy[MAX_CATALOG_KEY_LEN];
    TextBuffer outputs[5];

    if (text_copy(copy, sizeof(copy), key) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    outputs[0] = (TextBuffer){.data = component, .capacity = component_size};
    outputs[1] = (TextBuffer){.data = tool, .capacity = tool_size};
    outputs[2] = (TextBuffer){.data = host, .capacity = host_size};
    outputs[3] = (TextBuffer){.data = target, .capacity = target_size};
    outputs[4] = (TextBuffer){.data = field, .capacity = field_size};

    return text_split_exact(copy, '.', outputs, 5) == CUP_OK ? CUP_OK : CUP_ERR_CATALOG;
}

typedef struct {
    const char *name;
    unsigned bit;
    size_t offset;
    size_t capacity;
} CatalogField;

#define MANIFEST_FIELD(name_value, bit_value, member) \
    {name_value, \
     bit_value, \
     offsetof(PackageCatalogEntry, member), \
     sizeof(((PackageCatalogEntry *)0)->member)}

static const CatalogField PACKAGE_FIELDS[] = {
    MANIFEST_FIELD("stable_version", FIELD_STABLE_VERSION, stable_version),
    MANIFEST_FIELD("available_versions", FIELD_AVAILABLE_VERSIONS, available_versions),
    MANIFEST_FIELD("default_format", FIELD_DEFAULT_FORMAT, default_format),
    MANIFEST_FIELD("formats", FIELD_FORMATS, formats),
    MANIFEST_FIELD("url_template", FIELD_URL_TEMPLATE, url_template),
    MANIFEST_FIELD("checksum_url_template", FIELD_CHECKSUM_URL, checksum_url_template)};

/* Assign one validated key/value record without exposing a partially assembled package. */
static const CatalogField *find_catalog_field(const char *name) {
    size_t i;

    for (i = 0; i < sizeof(PACKAGE_FIELDS) / sizeof(PACKAGE_FIELDS[0]); ++i) {
        if (strcmp(PACKAGE_FIELDS[i].name, name) == 0) {
            return &PACKAGE_FIELDS[i];
        }
    }
    return NULL;
}

static CupError set_package_field(PackageCatalogEntry *package,
                                  const char *field,
                                  const char *value) {
    const CatalogField *descriptor;
    char *destination;

    descriptor = find_catalog_field(field);
    if (descriptor == NULL) {
        fprintf(stderr, "Error: unknown catalog field '%s'.\n", field);
        return CUP_ERR_CATALOG;
    }
    if ((package->field_mask & descriptor->bit) != 0) {
        fprintf(stderr, "Error: duplicate catalog field '%s'.\n", field);
        return CUP_ERR_CATALOG;
    }

    destination = (char *)package + descriptor->offset;
    if (text_copy(destination, descriptor->capacity, value) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    package->field_mask |= descriptor->bit;
    return CUP_OK;
}

static CupError parse_catalog_line(PackageCatalog *catalog, char *line) {
    char key[MAX_CATALOG_KEY_LEN];
    char value[MAX_CATALOG_URL_LEN];
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char host[MAX_PLATFORM_LEN];
    char target[MAX_PLATFORM_LEN];
    char field[MAX_IDENTIFIER_LEN];
    PackageCatalogEntry *package;
    int index;

    if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }
    if (parse_package_key(key,
                          component,
                          sizeof(component),
                          tool,
                          sizeof(tool),
                          host,
                          sizeof(host),
                          target,
                          sizeof(target),
                          field,
                          sizeof(field)) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }
    if (registry_validate_component(component) != CUP_OK ||
        registry_validate_tool(component, tool) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }
    if (platform_validate(host) != CUP_OK || platform_validate(target) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    index = find_package_index(catalog, component, tool, host, target);
    if (index == -1) {
        CupError err = append_package(catalog, component, tool, host, target, &package);

        if (err != CUP_OK) {
            return err;
        }
    } else {
        package = &catalog->packages[index];
    }

    return set_package_field(package, field, value);
}

/* Cross-record validation after the full file has been parsed. */
#define PLACEHOLDER_TOOL (1u << 0)
#define PLACEHOLDER_HOST (1u << 1)
#define PLACEHOLDER_TARGET (1u << 2)
#define PLACEHOLDER_VERSION (1u << 3)
#define PLACEHOLDER_FORMAT (1u << 4)
#define PACKAGE_URL_REQUIRED_PLACEHOLDERS \
    (PLACEHOLDER_HOST | PLACEHOLDER_TARGET | PLACEHOLDER_VERSION | PLACEHOLDER_FORMAT)
#define CHECKSUM_URL_REQUIRED_PLACEHOLDERS \
    (PLACEHOLDER_HOST | PLACEHOLDER_TARGET | PLACEHOLDER_VERSION)
#define PACKAGE_URL_ALLOWED_PLACEHOLDERS (PLACEHOLDER_TOOL | PACKAGE_URL_REQUIRED_PLACEHOLDERS)
#define CHECKSUM_URL_ALLOWED_PLACEHOLDERS (PLACEHOLDER_TOOL | CHECKSUM_URL_REQUIRED_PLACEHOLDERS)

static CupError validate_value_list(const char *value, const char *expected, int *contains) {
    char copy[MAX_CATALOG_VALUE_LEN];
    char *parts[(MAX_CATALOG_VALUE_LEN + 1) / 2];
    char *cursor;
    size_t count = 0;

    if (contains != NULL) {
        *contains = 0;
    }

    if (text_copy(copy, sizeof(copy), value) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    cursor = copy;
    while (cursor != NULL) {
        char *separator = strchr(cursor, ',');
        char *part;
        size_t i;

        if (separator != NULL) {
            *separator = '\0';
        }

        part = text_trim(cursor);
        if (!path_is_safe_identifier(part)) {
            return CUP_ERR_CATALOG;
        }

        for (i = 0; i < count; ++i) {
            if (strcmp(parts[i], part) == 0) {
                fprintf(stderr, "Error: catalog list contains duplicate value '%s'.\n", part);
                return CUP_ERR_CATALOG;
            }
        }
        parts[count++] = part;

        if (contains != NULL && expected != NULL && strcmp(part, expected) == 0) {
            *contains = 1;
        }

        cursor = separator == NULL ? NULL : separator + 1;
    }

    return CUP_OK;
}

/* URL templates and concrete package tuples are validated as closed schemas. */
static unsigned placeholder_bit(const char *start, size_t length) {
    static const struct {
        const char *name;
        unsigned bit;
    } placeholders[] = {
        {"{tool}", PLACEHOLDER_TOOL},
        {"{host_platform}", PLACEHOLDER_HOST},
        {"{target_platform}", PLACEHOLDER_TARGET},
        {"{version}", PLACEHOLDER_VERSION},
        {"{format}", PLACEHOLDER_FORMAT},
    };
    size_t i;

    for (i = 0; i < sizeof(placeholders) / sizeof(placeholders[0]); ++i) {
        if (strlen(placeholders[i].name) == length &&
            strncmp(start, placeholders[i].name, length) == 0) {
            return placeholders[i].bit;
        }
    }

    return 0;
}

static CupError validate_url_template(const char *url,
                                      unsigned allowed,
                                      unsigned required,
                                      const char *field_name) {
    const char *cursor;
    unsigned seen = 0;

    if (strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "Error: catalog URL templates must use HTTPS.\n");
        return CUP_ERR_CATALOG;
    }

    cursor = url;
    while (*cursor != '\0') {
        const char *end;
        unsigned bit;

        if (isspace((unsigned char)*cursor)) {
            fprintf(stderr, "Error: catalog field '%s' contains whitespace.\n", field_name);
            return CUP_ERR_CATALOG;
        }

        if (*cursor == '}') {
            return CUP_ERR_CATALOG;
        }

        if (*cursor != '{') {
            cursor++;
            continue;
        }

        end = strchr(cursor, '}');
        if (end == NULL) {
            return CUP_ERR_CATALOG;
        }

        bit = placeholder_bit(cursor, (size_t)(end - cursor + 1));
        if (bit == 0 || (allowed & bit) == 0) {
            fprintf(stderr, "Error: invalid placeholder in catalog field '%s'.\n", field_name);
            return CUP_ERR_CATALOG;
        }
        seen |= bit;
        cursor = end + 1;
    }

    if ((seen & required) != required) {
        fprintf(
            stderr, "Error: catalog field '%s' is missing required placeholders.\n", field_name);
        return CUP_ERR_CATALOG;
    }

    return CUP_OK;
}

static CupError validate_archive_format_list(const char *value) {
    char copy[MAX_CATALOG_VALUE_LEN];
    char *cursor;

    if (text_copy(copy, sizeof(copy), value) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }

    cursor = copy;
    while (cursor != NULL) {
        PackageArchiveFormat format;
        char *separator = strchr(cursor, ',');
        char *part;

        if (separator != NULL) {
            *separator = '\0';
        }
        part = text_trim(cursor);
        if (package_archive_parse_format(part, &format) != CUP_OK) {
            fprintf(stderr, "Error: catalog contains unsupported archive format '%s'.\n", part);
            return CUP_ERR_CATALOG;
        }
        cursor = separator == NULL ? NULL : separator + 1;
    }

    return CUP_OK;
}

static CupError validate_catalog_entry(const PackageCatalogEntry *package) {
    PackageArchiveFormat default_format;
    int contains;

    if (package->field_mask != REQUIRED_FIELDS) {
        fprintf(stderr,
                "Error: catalog package '%s.%s.%s.%s' is missing "
                "one or more required fields.\n",
                package->component,
                package->tool,
                package->host_platform,
                package->target_platform);
        return CUP_ERR_CATALOG;
    }
    if (!path_is_safe_identifier(package->stable_version) ||
        package_archive_parse_format(package->default_format, &default_format) != CUP_OK) {
        fprintf(stderr,
                "Error: catalog package '%s.%s.%s.%s' contains an "
                "invalid version or format identifier.\n",
                package->component,
                package->tool,
                package->host_platform,
                package->target_platform);
        return CUP_ERR_CATALOG;
    }

    if (validate_value_list(package->available_versions, package->stable_version, &contains) !=
            CUP_OK ||
        !contains) {
        fprintf(stderr,
                "Error: stable_version is not listed in "
                "available_versions for '%s.%s.%s.%s'.\n",
                package->component,
                package->tool,
                package->host_platform,
                package->target_platform);
        return CUP_ERR_CATALOG;
    }

    if (validate_archive_format_list(package->formats) != CUP_OK ||
        validate_value_list(package->formats, package->default_format, &contains) != CUP_OK ||
        !contains) {
        fprintf(stderr,
                "Error: default_format is not listed in formats for "
                "'%s.%s.%s.%s'.\n",
                package->component,
                package->tool,
                package->host_platform,
                package->target_platform);
        return CUP_ERR_CATALOG;
    }

    if (validate_url_template(package->url_template,
                              PACKAGE_URL_ALLOWED_PLACEHOLDERS,
                              PACKAGE_URL_REQUIRED_PLACEHOLDERS,
                              "url_template") != CUP_OK) {
        return CUP_ERR_CATALOG;
    }
    return validate_url_template(package->checksum_url_template,
                                 CHECKSUM_URL_ALLOWED_PLACEHOLDERS,
                                 CHECKSUM_URL_REQUIRED_PLACEHOLDERS,
                                 "checksum_url_template");
}

/* Load from installed official assets or the explicit development fallback, never both. */
CupError package_catalog_load_path(PackageCatalog *catalog,
                                   const char *path,
                                   PackageCatalogSource source) {
    FILE *file;
    CupError err;
    char line[MAX_CATALOG_LINE_LEN];
    size_t line_number = 0;
    size_t i;

    if (catalog == NULL || text_is_empty(path) ||
        (source != PACKAGE_CATALOG_SOURCE_INSTALLED &&
         source != PACKAGE_CATALOG_SOURCE_DEVELOPMENT)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package_catalog_free(catalog);
    catalog->source = source;

    if (text_copy(catalog->path, sizeof(catalog->path), path) != CUP_OK) {
        package_catalog_free(catalog);
        return CUP_ERR_CATALOG;
    }

    errno = 0;
    file = fopen(path, "r");
    if (file == NULL) {
        package_catalog_free(catalog);
        return errno == ENOENT ? CUP_ERR_CATALOG : CUP_ERR_FILESYSTEM;
    }

    while (1) {
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            if (err == CUP_ERR_FILESYSTEM) {
                fprintf(stderr,
                        "Error: could not read package catalog near line %zu.\n",
                        line_number + 1);
            } else {
                fprintf(stderr, "Error: invalid package catalog line %zu.\n", line_number);
            }
            fclose(file);
            package_catalog_free(catalog);
            return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_CATALOG;
        }

        if (!has_line) {
            break;
        }

        err = parse_catalog_line(catalog, line);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: invalid package catalog line %zu.\n", line_number);
            fclose(file);
            package_catalog_free(catalog);
            return err == CUP_ERR_TEMPORARY ? err : CUP_ERR_CATALOG;
        }
    }

    if (fclose(file) != 0) {
        package_catalog_free(catalog);
        return CUP_ERR_FILESYSTEM;
    }
    if (catalog->count == 0) {
        package_catalog_free(catalog);
        return CUP_ERR_CATALOG;
    }

    for (i = 0; i < catalog->count; ++i) {
        if (validate_catalog_entry(&catalog->packages[i]) != CUP_OK) {
            package_catalog_free(catalog);
            return CUP_ERR_CATALOG;
        }
    }

    return CUP_OK;
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    char path[MAX_PATH_LEN];
    CupError err;

    err = layout_get_package_catalog_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    return package_catalog_load_path(catalog, path, PACKAGE_CATALOG_SOURCE_INSTALLED);
}

CupError package_catalog_load_development(PackageCatalog *catalog) {
    return package_catalog_load_path(
        catalog, DEVELOPMENT_MANIFEST_PATH, PACKAGE_CATALOG_SOURCE_DEVELOPMENT);
}

CupError package_catalog_load(PackageCatalog *catalog) {
    char installed[MAX_PATH_LEN];
    CupError err;
    int exists;

    if (catalog == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_package_catalog_path(installed, sizeof(installed));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(installed, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists) {
        return package_catalog_load_path(catalog, installed, PACKAGE_CATALOG_SOURCE_INSTALLED);
    }

    err = system_path_exists(DEVELOPMENT_MANIFEST_PATH, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (exists) {
        return package_catalog_load_development(catalog);
    }

    fprintf(stderr,
            "Error: package catalog not found at '%s' or './%s'.\n",
            installed,
            DEVELOPMENT_MANIFEST_PATH);
    return CUP_ERR_CATALOG;
}

/* Read-only catalog queries preserve the distinction between package, version and format
 * availability. */
static const PackageCatalogEntry *find_package(const PackageCatalog *catalog,
                                               const char *component,
                                               const char *tool,
                                               const char *host,
                                               const char *target) {
    int index = find_package_index(catalog, component, tool, host, target);

    return index == -1 ? NULL : &catalog->packages[index];
}

static const PackageCatalogEntry *require_package(const PackageCatalog *catalog,
                                                  const char *component,
                                                  const char *tool,
                                                  const char *host,
                                                  const char *target) {
    const PackageCatalogEntry *package = find_package(catalog, component, tool, host, target);

    if (package == NULL) {
        fprintf(stderr,
                "Error: tool '%s' for component '%s' is not "
                "configured for host '%s', target '%s'.\n",
                tool,
                component,
                host,
                target);
    }

    return package;
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host,
                                        const char *target) {
    const PackageCatalogEntry *package;

    if (catalog == NULL || buffer == NULL || size == 0 || text_is_empty(component) ||
        text_is_empty(tool) || text_is_empty(host) || text_is_empty(target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }

    return text_copy(buffer, size, package->stable_version);
}

CupError package_catalog_is_stable(const PackageCatalog *catalog,
                                   const char *component,
                                   const char *tool,
                                   const char *host,
                                   const char *target,
                                   const char *version,
                                   int *is_stable) {
    const PackageCatalogEntry *package;

    if (is_stable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_stable = 0;
    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target) || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = find_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }

    *is_stable = strcmp(package->stable_version, version) == 0;
    return CUP_OK;
}

CupError package_catalog_has_package(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host,
                                     const char *target,
                                     int *is_available) {
    if (is_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_available = 0;
    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_available = find_package(catalog, component, tool, host, target) != NULL;
    return CUP_OK;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host,
                                     const char *target,
                                     const char *version,
                                     int *is_available) {
    const PackageCatalogEntry *package;

    if (is_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_available = 0;
    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target) || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = find_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }

    return validate_value_list(package->available_versions, version, is_available);
}

CupError package_catalog_get_default_format(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host,
                                            const char *target) {
    const PackageCatalogEntry *package;

    if (catalog == NULL || buffer == NULL || size == 0 || text_is_empty(component) ||
        text_is_empty(tool) || text_is_empty(host) || text_is_empty(target)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }

    return text_copy(buffer, size, package->default_format);
}

CupError package_catalog_has_format(const PackageCatalog *catalog,
                                    const char *component,
                                    const char *tool,
                                    const char *host,
                                    const char *target,
                                    const char *format,
                                    int *is_supported) {
    const PackageCatalogEntry *package;

    if (is_supported == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_supported = 0;
    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target) || text_is_empty(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = find_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }

    return validate_value_list(package->formats, format, is_supported);
}

/* Expand only known template placeholders after the selected package tuple is concrete. */
static CupError replace_placeholder(
    char *buffer, size_t size, const char *input, const char *placeholder, const char *value) {
    const char *cursor;
    const char *match;
    size_t placeholder_length;
    size_t value_length;
    size_t written = 0;

    if (buffer == NULL || size == 0 || input == NULL || text_is_empty(placeholder) ||
        value == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = input;
    placeholder_length = strlen(placeholder);
    value_length = strlen(value);
    while ((match = strstr(cursor, placeholder)) != NULL) {
        size_t prefix_length = (size_t)(match - cursor);

        if (prefix_length >= size - written) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buffer + written, cursor, prefix_length);
        written += prefix_length;

        if (value_length >= size - written) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buffer + written, value, value_length);
        written += value_length;
        cursor = match + placeholder_length;
    }

    {
        size_t tail_length = strlen(cursor);
        if (tail_length >= size - written) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buffer + written, cursor, tail_length + 1);
    }
    return CUP_OK;
}

static CupError expand_package_url(const char *template_value,
                                   char *buffer,
                                   size_t size,
                                   const char *tool,
                                   const char *host,
                                   const char *target,
                                   const char *version,
                                   const char *format) {
    char step1[MAX_CATALOG_URL_LEN];
    char step2[MAX_CATALOG_URL_LEN];
    char step3[MAX_CATALOG_URL_LEN];
    char step4[MAX_CATALOG_URL_LEN];
    CupError err;

    err = replace_placeholder(step1, sizeof(step1), template_value, "{tool}", tool);
    if (err != CUP_OK) {
        return err == CUP_ERR_BUFFER_TOO_SMALL ? err : CUP_ERR_CATALOG;
    }
    err = replace_placeholder(step2, sizeof(step2), step1, "{host_platform}", host);
    if (err != CUP_OK) {
        return err == CUP_ERR_BUFFER_TOO_SMALL ? err : CUP_ERR_CATALOG;
    }
    err = replace_placeholder(step3, sizeof(step3), step2, "{target_platform}", target);
    if (err != CUP_OK) {
        return err == CUP_ERR_BUFFER_TOO_SMALL ? err : CUP_ERR_CATALOG;
    }
    err = replace_placeholder(step4, sizeof(step4), step3, "{version}", version);
    if (err != CUP_OK) {
        return err == CUP_ERR_BUFFER_TOO_SMALL ? err : CUP_ERR_CATALOG;
    }

    err = replace_placeholder(buffer, size, step4, "{format}", format == NULL ? "" : format);
    if (err != CUP_OK) {
        return err == CUP_ERR_BUFFER_TOO_SMALL ? err : CUP_ERR_CATALOG;
    }

    if (strchr(buffer, '{') != NULL || strchr(buffer, '}') != NULL ||
        strncmp(buffer, "https://", 8) != 0) {
        return CUP_ERR_CATALOG;
    }
    return CUP_OK;
}

CupError package_catalog_build_url(const PackageCatalog *catalog,
                                   char *buffer,
                                   size_t size,
                                   const char *component,
                                   const char *tool,
                                   const char *host,
                                   const char *target,
                                   const char *version,
                                   const char *format) {
    const PackageCatalogEntry *package;

    if (catalog == NULL || buffer == NULL || size == 0 || text_is_empty(component) ||
        text_is_empty(tool) || text_is_empty(host) || text_is_empty(target) ||
        text_is_empty(version) || text_is_empty(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }
    return expand_package_url(
        package->url_template, buffer, size, tool, host, target, version, format);
}

CupError package_catalog_build_checksum_url(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host,
                                            const char *target,
                                            const char *version) {
    const PackageCatalogEntry *package;

    if (catalog == NULL || buffer == NULL || size == 0 || text_is_empty(component) ||
        text_is_empty(tool) || text_is_empty(host) || text_is_empty(target) ||
        text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package = require_package(catalog, component, tool, host, target);
    if (package == NULL) {
        return CUP_ERR_CATALOG;
    }
    return expand_package_url(
        package->checksum_url_template, buffer, size, tool, host, target, version, "");
}
