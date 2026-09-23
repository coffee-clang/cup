/* Load and query the concrete package availability snapshot in catalog.cfg. */

#include "package_catalog.h"

#include "checksum.h"
#include "download.h"
#include "filesystem.h"
#include "layout.h"
#include "package_archive.h"
#include "package_selector.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"
#include "version.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKAGE_FIELD_COMPONENT (1u << 0)
#define PACKAGE_FIELD_TOOL (1u << 1)
#define PACKAGE_FIELD_HOST (1u << 2)
#define PACKAGE_FIELD_TARGET (1u << 3)
#define PACKAGE_FIELD_VERSION (1u << 4)
#define PACKAGE_FIELD_STABLE (1u << 5)
#define PACKAGE_FIELD_REVISION_REASON (1u << 6)
#define PACKAGE_REQUIRED_FIELDS \
    (PACKAGE_FIELD_COMPONENT | PACKAGE_FIELD_TOOL | PACKAGE_FIELD_HOST | PACKAGE_FIELD_TARGET | \
     PACKAGE_FIELD_VERSION | PACKAGE_FIELD_STABLE)

#define ARTIFACT_FIELD_FORMAT (1u << 0)
#define ARTIFACT_FIELD_URL (1u << 1)
#define ARTIFACT_FIELD_SHA256 (1u << 2)
#define ARTIFACT_REQUIRED_FIELDS \
    (ARTIFACT_FIELD_FORMAT | ARTIFACT_FIELD_URL | ARTIFACT_FIELD_SHA256)

#define DEVELOPMENT_CATALOG_PATH "config/catalog.cfg"

static int ascii_is_alnum(unsigned char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

static int ascii_token_is_safe(const char *value, size_t capacity) {
    const unsigned char *cursor;
    size_t length;

    if (text_is_empty(value)) {
        return 0;
    }
    length = strlen(value);
    if (length >= capacity || !ascii_is_alnum((unsigned char)value[0])) {
        return 0;
    }
    for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        if (ascii_is_alnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '+' ||
            *cursor == '-' || *cursor == ':') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int text_value_is_safe(const char *value, size_t capacity) {
    const unsigned char *cursor;
    size_t length;

    if (text_is_empty(value)) {
        return 0;
    }
    length = strlen(value);
    if (length >= capacity) {
        return 0;
    }
    for (cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        if (*cursor < 0x20 || *cursor > 0x7e) {
            return 0;
        }
    }
    return 1;
}

static int catalog_url_is_valid(const char *url) {
    const unsigned char *cursor;

    if (text_is_empty(url) || strlen(url) >= MAX_CATALOG_URL_LEN) {
        return 0;
    }
    if (strncmp(url, "https://", 8) != 0) {
        return download_insecure_loopback_is_allowed(url);
    }
    if (url[8] == '\0') {
        return 0;
    }
    for (cursor = (const unsigned char *)url; *cursor != '\0'; ++cursor) {
        if (*cursor < 0x21 || *cursor > 0x7e) {
            return 0;
        }
    }
    return 1;
}

static int parse_uint64_canonical(const char *value, uint64_t *result) {
    unsigned long long parsed;
    char *end = NULL;

    if (text_is_empty(value) || result == NULL ||
        (value[0] == '0' && value[1] != '\0')) {
        return 0;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return 0;
    }
    *result = (uint64_t)parsed;
    return 1;
}

static CupError parse_index(const char *value, const char **rest, size_t *index) {
    unsigned long long parsed;
    char digits[32];
    size_t length = 0;
    char *end = NULL;

    if (text_is_empty(value) || rest == NULL || index == NULL) {
        return CUP_ERR_CATALOG;
    }
    while (value[length] >= '0' && value[length] <= '9') {
        if (length + 1 >= sizeof(digits)) {
            return CUP_ERR_CATALOG;
        }
        digits[length] = value[length];
        length++;
    }
    if (length == 0 || value[length] != '.' || (length > 1 && digits[0] == '0')) {
        return CUP_ERR_CATALOG;
    }
    digits[length] = '\0';
    errno = 0;
    parsed = strtoull(digits, &end, 10);
    if (errno != 0 || end == digits || *end != '\0' || parsed > SIZE_MAX) {
        return CUP_ERR_CATALOG;
    }
    *index = (size_t)parsed;
    *rest = value + length + 1u;
    return CUP_OK;
}

void package_catalog_init(PackageCatalog *catalog) {
    if (catalog != NULL) {
        memset(catalog, 0, sizeof(*catalog));
    }
}

void package_catalog_free(PackageCatalog *catalog) {
    size_t i;

    if (catalog == NULL) {
        return;
    }
    for (i = 0; i < catalog->count; ++i) {
        free(catalog->packages[i].artifacts);
    }
    free(catalog->packages);
    package_catalog_init(catalog);
}

static CupError ensure_package(PackageCatalog *catalog,
                               size_t index,
                               PackageCatalogEntry **entry) {
    PackageCatalogEntry *packages;
    size_t capacity;

    if (catalog == NULL || entry == NULL || index > catalog->count) {
        return CUP_ERR_CATALOG;
    }
    if (index == catalog->count) {
        if (catalog->count == catalog->capacity) {
            capacity = catalog->capacity == 0 ? 16u : catalog->capacity * 2u;
            if (capacity < catalog->capacity || capacity > SIZE_MAX / sizeof(*packages)) {
                return CUP_ERR_TEMPORARY;
            }
            packages = realloc(catalog->packages, capacity * sizeof(*packages));
            if (packages == NULL) {
                return CUP_ERR_TEMPORARY;
            }
            catalog->packages = packages;
            catalog->capacity = capacity;
        }
        memset(&catalog->packages[catalog->count], 0, sizeof(*catalog->packages));
        catalog->count++;
    }
    *entry = &catalog->packages[index];
    return CUP_OK;
}

static CupError ensure_artifact(PackageCatalogEntry *entry,
                                size_t index,
                                PackageCatalogArtifact **artifact) {
    PackageCatalogArtifact *items;
    size_t capacity;

    if (entry == NULL || artifact == NULL || index > entry->artifact_count) {
        return CUP_ERR_CATALOG;
    }
    if (index == entry->artifact_count) {
        if (entry->artifact_count == entry->artifact_capacity) {
            capacity = entry->artifact_capacity == 0 ? 4u : entry->artifact_capacity * 2u;
            if (capacity < entry->artifact_capacity || capacity > SIZE_MAX / sizeof(*items)) {
                return CUP_ERR_TEMPORARY;
            }
            items = realloc(entry->artifacts, capacity * sizeof(*items));
            if (items == NULL) {
                return CUP_ERR_TEMPORARY;
            }
            entry->artifacts = items;
            entry->artifact_capacity = capacity;
        }
        memset(&entry->artifacts[entry->artifact_count], 0, sizeof(*entry->artifacts));
        entry->artifact_count++;
    }
    *artifact = &entry->artifacts[index];
    return CUP_OK;
}

static CupError set_token_field(char *destination,
                                size_t size,
                                unsigned *mask,
                                unsigned bit,
                                const char *value,
                                size_t token_capacity) {
    if ((*mask & bit) != 0 || !ascii_token_is_safe(value, token_capacity) ||
        text_copy(destination, size, value) != CUP_OK) {
        return CUP_ERR_CATALOG;
    }
    *mask |= bit;
    return CUP_OK;
}

static CupError parse_artifact_field(PackageCatalogEntry *entry,
                                     const char *suffix,
                                     const char *value) {
    PackageCatalogArtifact *artifact;
    const char *field;
    size_t index;
    CupError err;

    err = parse_index(suffix, &field, &index);
    if (err != CUP_OK) {
        return err;
    }
    err = ensure_artifact(entry, index, &artifact);
    if (err != CUP_OK) {
        return err;
    }

    if (strcmp(field, "format") == 0) {
        return set_token_field(artifact->format,
                               sizeof(artifact->format),
                               &artifact->field_mask,
                               ARTIFACT_FIELD_FORMAT,
                               value,
                               sizeof(artifact->format));
    }
    if (strcmp(field, "url") == 0) {
        if ((artifact->field_mask & ARTIFACT_FIELD_URL) != 0 || !catalog_url_is_valid(value) ||
            text_copy(artifact->url, sizeof(artifact->url), value) != CUP_OK) {
            return CUP_ERR_CATALOG;
        }
        artifact->field_mask |= ARTIFACT_FIELD_URL;
        return CUP_OK;
    }
    if (strcmp(field, "sha256") == 0) {
        if ((artifact->field_mask & ARTIFACT_FIELD_SHA256) != 0 ||
            !checksum_digest_is_canonical(value) ||
            text_copy(artifact->sha256, sizeof(artifact->sha256), value) != CUP_OK) {
            return CUP_ERR_CATALOG;
        }
        artifact->field_mask |= ARTIFACT_FIELD_SHA256;
        return CUP_OK;
    }
    return CUP_ERR_CATALOG;
}

static CupError parse_package_field(PackageCatalog *catalog,
                                    const char *suffix,
                                    const char *value) {
    PackageCatalogEntry *entry;
    const char *field;
    size_t index;
    CupError err;

    err = parse_index(suffix, &field, &index);
    if (err != CUP_OK) {
        return err;
    }
    err = ensure_package(catalog, index, &entry);
    if (err != CUP_OK) {
        return err;
    }

    if (strncmp(field, "artifact.", 9) == 0) {
        return parse_artifact_field(entry, field + 9, value);
    }
    if (strcmp(field, "component") == 0) {
        return set_token_field(entry->component, sizeof(entry->component), &entry->field_mask,
                               PACKAGE_FIELD_COMPONENT, value, sizeof(entry->component));
    }
    if (strcmp(field, "tool") == 0) {
        return set_token_field(entry->tool, sizeof(entry->tool), &entry->field_mask,
                               PACKAGE_FIELD_TOOL, value, sizeof(entry->tool));
    }
    if (strcmp(field, "host") == 0) {
        return set_token_field(entry->host_platform, sizeof(entry->host_platform), &entry->field_mask,
                               PACKAGE_FIELD_HOST, value, sizeof(entry->host_platform));
    }
    if (strcmp(field, "target") == 0) {
        return set_token_field(entry->target_platform, sizeof(entry->target_platform),
                               &entry->field_mask, PACKAGE_FIELD_TARGET, value,
                               sizeof(entry->target_platform));
    }
    if (strcmp(field, "version") == 0) {
        return set_token_field(entry->version, sizeof(entry->version), &entry->field_mask,
                               PACKAGE_FIELD_VERSION, value, sizeof(entry->version));
    }
    if (strcmp(field, "revision_reason") == 0) {
        if ((entry->field_mask & PACKAGE_FIELD_REVISION_REASON) != 0 ||
            !text_value_is_safe(value, sizeof(entry->revision_reason)) ||
            text_copy(entry->revision_reason, sizeof(entry->revision_reason), value) != CUP_OK) {
            return CUP_ERR_CATALOG;
        }
        entry->field_mask |= PACKAGE_FIELD_REVISION_REASON;
        return CUP_OK;
    }
    if (strcmp(field, "stable") == 0) {
        if ((entry->field_mask & PACKAGE_FIELD_STABLE) != 0 ||
            (strcmp(value, "true") != 0 && strcmp(value, "false") != 0)) {
            return CUP_ERR_CATALOG;
        }
        entry->stable = strcmp(value, "true") == 0;
        entry->field_mask |= PACKAGE_FIELD_STABLE;
        return CUP_OK;
    }
    return CUP_ERR_CATALOG;
}

static int same_tuple(const PackageCatalogEntry *left, const PackageCatalogEntry *right) {
    return strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0;
}

static int entry_has_supported_artifact(const PackageCatalogEntry *entry) {
    size_t i;

    for (i = 0; i < entry->artifact_count; ++i) {
        PackageArchiveFormat format;
        if (package_archive_parse_format(entry->artifacts[i].format, &format) == CUP_OK) {
            return 1;
        }
    }
    return 0;
}

static int entry_version_contract_is_operational(const PackageCatalogEntry *entry) {
    char base[MAX_IDENTIFIER_LEN];
    int revised;
    int has_reason;

    if (package_release_validate_concrete(entry->version) != CUP_OK ||
        package_release_base(entry->version, base, sizeof(base)) != CUP_OK) {
        return 0;
    }
    revised = strcmp(base, entry->version) != 0;
    has_reason = (entry->field_mask & PACKAGE_FIELD_REVISION_REASON) != 0;
    return revised == has_reason;
}

static CupError classify_operational(PackageCatalogEntry *entry) {
    int domain_operational = registry_tool_is_operational(entry->component, entry->tool) &&
                             platform_is_supported(entry->host_platform) &&
                             platform_is_supported(entry->target_platform);
    int version_understood = package_release_validate_concrete(entry->version) == CUP_OK;

    if (domain_operational && version_understood && !entry_version_contract_is_operational(entry)) {
        return CUP_ERR_CATALOG;
    }
    entry->operational = domain_operational && version_understood &&
                         entry_version_contract_is_operational(entry) &&
                         entry_has_supported_artifact(entry);
    return CUP_OK;
}

static CupError validate_catalog(PackageCatalog *catalog, int have_revision, int have_update_url) {
    size_t i;
    size_t j;

    if (!have_revision || !have_update_url) {
        return CUP_ERR_CATALOG;
    }
    for (i = 0; i < catalog->count; ++i) {
        PackageCatalogEntry *entry = &catalog->packages[i];

        if ((entry->field_mask & PACKAGE_REQUIRED_FIELDS) != PACKAGE_REQUIRED_FIELDS ||
            entry->artifact_count == 0) {
            return CUP_ERR_CATALOG;
        }
        for (j = 0; j < entry->artifact_count; ++j) {
            size_t k;
            if (entry->artifacts[j].field_mask != ARTIFACT_REQUIRED_FIELDS) {
                return CUP_ERR_CATALOG;
            }
            for (k = j + 1; k < entry->artifact_count; ++k) {
                if (strcmp(entry->artifacts[j].format, entry->artifacts[k].format) == 0) {
                    return CUP_ERR_CATALOG;
                }
            }
        }
        if (classify_operational(entry) != CUP_OK) {
            return CUP_ERR_CATALOG;
        }
        for (j = i + 1; j < catalog->count; ++j) {
            PackageCatalogEntry *other = &catalog->packages[j];
            if (!same_tuple(entry, other)) {
                continue;
            }
            if (strcmp(entry->version, other->version) == 0 || (entry->stable && other->stable)) {
                return CUP_ERR_CATALOG;
            }
        }
    }

    /* A stable record understood by this CUP must be the maximum understood version. A future
     * non-operational stable remains the materialized stable and deliberately suppresses fallback. */
    for (i = 0; i < catalog->count; ++i) {
        PackageCatalogEntry *stable = &catalog->packages[i];
        if (!stable->stable || !stable->operational) {
            continue;
        }
        for (j = 0; j < catalog->count; ++j) {
            PackageCatalogEntry *other = &catalog->packages[j];
            int compared;
            if (!other->operational || !same_tuple(stable, other)) {
                continue;
            }
            if (package_release_compare(stable->version, other->version, &compared) != CUP_OK ||
                compared < 0) {
                return CUP_ERR_CATALOG;
            }
        }
    }
    return CUP_OK;
}

CupError package_catalog_load_path(PackageCatalog *catalog, const char *path) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupError err;
    char line[MAX_CATALOG_LINE_LEN];
    char key[MAX_CATALOG_KEY_LEN];
    char value[MAX_CATALOG_VALUE_LEN];
    char expected_format[32];
    int missing;
    int has_line;
    int have_revision = 0;
    int have_update_url = 0;

    if (catalog == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    package_catalog_free(catalog);
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, MAX_PERSISTENT_METADATA_BYTES, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_CATALOG;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    if (err != CUP_OK) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_CATALOG;
    }
    if (snprintf(expected_format, sizeof(expected_format), "format=%d", CUP_PACKAGE_CATALOG_FORMAT) < 0) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_CATALOG;
    }
    err = text_document_read_raw_line(&reader, line, sizeof(line), &has_line);
    if (err != CUP_OK || !has_line || strcmp(line, expected_format) != 0) {
        CupError format_error = CUP_ERR_CATALOG;
        if (err == CUP_OK && has_line) {
            char format_key[MAX_CATALOG_KEY_LEN];
            char format_value[MAX_CATALOG_VALUE_LEN];
            uint64_t format_number;
            if (text_parse_key_value(line,
                                     format_key,
                                     sizeof(format_key),
                                     format_value,
                                     sizeof(format_value)) == CUP_OK &&
                strcmp(format_key, "format") == 0 &&
                parse_uint64_canonical(format_value, &format_number) &&
                format_number != CUP_PACKAGE_CATALOG_FORMAT) {
                format_error = CUP_ERR_NOT_AVAILABLE;
            }
        }
        filesystem_snapshot_release(&snapshot);
        return err == CUP_ERR_FILESYSTEM ? err : format_error;
    }

    while (1) {
        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK || !has_line) {
            break;
        }
        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err != CUP_OK) {
            break;
        }
        if (strcmp(key, "revision") == 0) {
            if (have_revision || !parse_uint64_canonical(value, &catalog->revision)) {
                err = CUP_ERR_CATALOG;
                break;
            }
            have_revision = 1;
        } else if (strcmp(key, "update_url") == 0) {
            if (have_update_url || !catalog_url_is_valid(value) ||
                text_copy(catalog->update_url, sizeof(catalog->update_url), value) != CUP_OK) {
                err = CUP_ERR_CATALOG;
                break;
            }
            have_update_url = 1;
        } else if (strncmp(key, "package.", 8) == 0) {
            err = parse_package_field(catalog, key + 8, value);
            if (err != CUP_OK) {
                break;
            }
        } else {
            err = CUP_ERR_CATALOG;
            break;
        }
    }
    if (err == CUP_OK && !has_line) {
        err = validate_catalog(catalog, have_revision, have_update_url);
    } else if (err == CUP_OK) {
        err = CUP_ERR_CATALOG;
    }
    if (err == CUP_OK) {
        catalog->identity = snapshot.identity;
        err = checksum_sha256_bytes(snapshot.data, snapshot.size, catalog->digest, sizeof(catalog->digest));
    }
    filesystem_snapshot_release(&snapshot);
    if (err != CUP_OK) {
        package_catalog_free(catalog);
        return err == CUP_ERR_TEMPORARY || err == CUP_ERR_FILESYSTEM ||
                       err == CUP_ERR_NOT_AVAILABLE
                   ? err
                   : CUP_ERR_CATALOG;
    }
    return CUP_OK;
}

CupError package_catalog_load_installed(PackageCatalog *catalog) {
    char path[MAX_PATH_LEN];
    CupError err;

    if (catalog == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    package_catalog_free(catalog);
    err = layout_get_package_catalog_path(path, sizeof(path));
    return err == CUP_OK ? package_catalog_load_path(catalog, path) : err;
}

CupError package_catalog_load_development(PackageCatalog *catalog) {
    return package_catalog_load_path(catalog, DEVELOPMENT_CATALOG_PATH);
}


CupError package_catalog_seed_runtime(void) {
#if CUP_VERSION_OFFICIAL
    return CUP_ERR_NOT_AVAILABLE;
#else
    PackageCatalog seed;
    char installed[MAX_PATH_LEN];
    CupError err;
    int exists;

    package_catalog_init(&seed);
    err = layout_get_package_catalog_path(installed, sizeof(installed));
    if (err == CUP_OK) {
        err = system_path_exists(installed, &exists);
    }
    if (err == CUP_OK && exists) {
        package_catalog_free(&seed);
        return CUP_OK;
    }
    if (err == CUP_OK) {
        err = package_catalog_load_development(&seed);
    }
    if (err == CUP_OK) {
        err = layout_ensure_config();
    }
    if (err == CUP_OK) {
        err = system_copy_file(DEVELOPMENT_CATALOG_PATH, installed);
    }
    package_catalog_free(&seed);
    return err;
#endif
}

CupError package_catalog_load(PackageCatalog *catalog) {
    char installed[MAX_PATH_LEN];
    CupError err;
    int exists;

    if (catalog == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    package_catalog_free(catalog);
    err = layout_get_package_catalog_path(installed, sizeof(installed));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(installed, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return package_catalog_load_path(catalog, installed);
    }
#if !CUP_VERSION_OFFICIAL
    err = system_path_exists(DEVELOPMENT_CATALOG_PATH, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return package_catalog_load_development(catalog);
    }
#endif
#if CUP_VERSION_OFFICIAL
    fprintf(stderr,
            "Error: installed package catalog not found at '%s'. Run 'cup repair' to restore it.\n",
            installed);
#else
    fprintf(stderr,
            "Error: package catalog not found at '%s' or './%s'.\n",
            installed,
            DEVELOPMENT_CATALOG_PATH);
#endif
    return CUP_ERR_CATALOG;
}

static const PackageCatalogEntry *find_release(const PackageCatalog *catalog,
                                               const char *component,
                                               const char *tool,
                                               const char *host,
                                               const char *target,
                                               const char *version) {
    size_t i;

    if (catalog == NULL || text_is_empty(component) || text_is_empty(tool) || text_is_empty(host) ||
        text_is_empty(target)) {
        return NULL;
    }
    for (i = 0; i < catalog->count; ++i) {
        const PackageCatalogEntry *entry = &catalog->packages[i];
        if (entry->operational && strcmp(entry->component, component) == 0 &&
            strcmp(entry->tool, tool) == 0 && strcmp(entry->host_platform, host) == 0 &&
            strcmp(entry->target_platform, target) == 0 &&
            (version == NULL || strcmp(entry->version, version) == 0)) {
            return entry;
        }
    }
    return NULL;
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform) {
    size_t i;

    if (buffer == NULL || size == 0 || catalog == NULL || text_is_empty(component) ||
        text_is_empty(tool) || text_is_empty(host_platform) || text_is_empty(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }
    buffer[0] = '\0';
    for (i = 0; i < catalog->count; ++i) {
        const PackageCatalogEntry *entry = &catalog->packages[i];
        if (entry->stable && entry->operational && strcmp(entry->component, component) == 0 &&
            strcmp(entry->tool, tool) == 0 && strcmp(entry->host_platform, host_platform) == 0 &&
            strcmp(entry->target_platform, target_platform) == 0) {
            return text_copy(buffer, size, entry->version);
        }
    }
    return CUP_ERR_NOT_AVAILABLE;
}

CupError package_catalog_is_stable(const PackageCatalog *catalog,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   int *is_stable) {
    const PackageCatalogEntry *entry;

    if (is_stable == NULL || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_stable = 0;
    entry = find_release(catalog, component, tool, host_platform, target_platform, version);
    if (entry == NULL) {
        return catalog == NULL ? CUP_ERR_INVALID_INPUT : CUP_OK;
    }
    *is_stable = entry->stable;
    return CUP_OK;
}

CupError package_catalog_has_package(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     int *is_available) {
    if (is_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_available = find_release(catalog, component, tool, host_platform, target_platform, NULL) != NULL;
    return catalog == NULL ? CUP_ERR_INVALID_INPUT : CUP_OK;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available) {
    if (is_available == NULL || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_available = find_release(catalog, component, tool, host_platform, target_platform, version) != NULL;
    return catalog == NULL ? CUP_ERR_INVALID_INPUT : CUP_OK;
}

CupError package_catalog_resolve_artifact(const PackageCatalog *catalog,
                                          const char *component,
                                          const char *tool,
                                          const char *host_platform,
                                          const char *target_platform,
                                          const char *version,
                                          const char *format,
                                          char *url,
                                          size_t url_size,
                                          char *artifact_sha256,
                                          size_t artifact_sha256_size) {
    const PackageCatalogEntry *entry;
    size_t i;
    CupError err;

    if (catalog == NULL || text_is_empty(version) || text_is_empty(format) || url == NULL ||
        url_size == 0 || artifact_sha256 == NULL || artifact_sha256_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    url[0] = artifact_sha256[0] = '\0';
    entry = find_release(catalog, component, tool, host_platform, target_platform, version);
    if (entry == NULL) {
        return CUP_ERR_NOT_AVAILABLE;
    }
    for (i = 0; i < entry->artifact_count; ++i) {
        const PackageCatalogArtifact *artifact = &entry->artifacts[i];
        if (strcmp(artifact->format, format) != 0) {
            continue;
        }
        err = text_copy(url, url_size, artifact->url);
        if (err == CUP_OK) {
            err = text_copy(artifact_sha256, artifact_sha256_size, artifact->sha256);
        }
        if (err != CUP_OK) {
            url[0] = artifact_sha256[0] = '\0';
        }
        return err;
    }
    return CUP_ERR_NOT_AVAILABLE;
}
