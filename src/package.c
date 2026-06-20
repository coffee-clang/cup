#include "package.h"

#include "entry.h"
#include "info.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// PACKAGE IDENTITY
CupError package_identity_init(PackageIdentity *identity, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *version) {
    CupError err;

    if (identity == NULL || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) ||
        is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_tool_for_component(component, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_platform(host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_platform(target_platform);
    if (err != CUP_OK) {
        return err;
    }

    if (!path_is_safe_identifier(version)) {
        fprintf(stderr, "Error: invalid package version '%s'.\n", version);
        return CUP_ERR_INVALID_RELEASE;
    }

    memset(identity, 0, sizeof(*identity));

    if (checked_snprintf(identity->component, sizeof(identity->component), "%s", component) != CUP_OK ||
        checked_snprintf(identity->tool, sizeof(identity->tool), "%s", tool) != CUP_OK ||
        checked_snprintf(identity->host_platform, sizeof(identity->host_platform), "%s", host_platform) != CUP_OK ||
        checked_snprintf(identity->target_platform, sizeof(identity->target_platform),
            "%s", target_platform) != CUP_OK ||
        checked_snprintf(identity->version, sizeof(identity->version), "%s", version) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

CupError package_identity_from_entry(PackageIdentity *identity, const char *component,
    const char *host_platform, const char *target_platform, const char *entry) {
    CupError err;
    char tool[MAX_NAME_LEN];
    char version[MAX_NAME_LEN];

    err = entry_parse(entry, tool, sizeof(tool), version, sizeof(version));
    if (err != CUP_OK) {
        return err;
    }

    return package_identity_init(identity, component, tool,
        host_platform, target_platform, version);
}

// PACKAGE VALIDATION
static CupError require_path_type(const char *path, int directory) {
    CupError err;
    int matches;
    long long file_size;

    if (directory) {
        err = system_is_directory(path, &matches);
        return err == CUP_OK && matches ? CUP_OK : CUP_ERR_VALIDATION;
    }

    err = system_is_regular_file(path, &matches);
    if (err != CUP_OK || !matches) {
        return CUP_ERR_VALIDATION;
    }

    err = system_file_size(path, &file_size);
    return err == CUP_OK && file_size > 0 ? CUP_OK : CUP_ERR_VALIDATION;
}

static CupError require_info_value(const PackageInfo *info, const char *key, const char *expected) {
    const char *actual;

    actual = info_get(info, key);
    if (actual == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr, "Error: package metadata field '%s' is missing or inconsistent.\n", key);
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_info_entries(const PackageInfo *info, const char *base_path) {
    const PackageInfoField *field;
    size_t cursor = 0;
    size_t count = 0;

    while ((field = info_next(info, "entry.", &cursor)) != NULL) {
        char path[MAX_PATH_LEN];
        int executable;

        if (!path_is_safe_relative(field->value) ||
            path_join_safe_relative(path, sizeof(path), base_path, field->value) != CUP_OK ||
            require_path_type(path, 0) != CUP_OK ||
            system_is_executable(path, &executable) != CUP_OK || !executable) {
            fprintf(stderr, "Error: package metadata entry '%s' points to invalid or non-executable file '%s'.\n",
                field->key, field->value);
            return CUP_ERR_VALIDATION;
        }

        count++;
    }

    if (count == 0) {
        fprintf(stderr, "Error: package metadata does not declare any entry.* file.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    PackageInfo info;
    CupError err;
    char info_path[MAX_PATH_LEN];

    if (is_empty_string(base_path) || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = require_path_type(base_path, 1);
    if (err != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILE);
    if (err != CUP_OK || require_path_type(info_path, 0) != CUP_OK) {
        fprintf(stderr, "Error: package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    info_init(&info);
    err = info_load(&info, info_path);
    if (err != CUP_OK) {
        info_free(&info);
        return CUP_ERR_VALIDATION;
    }

    if (require_info_value(&info, "package.component", identity->component) != CUP_OK ||
        require_info_value(&info, "package.tool", identity->tool) != CUP_OK ||
        require_info_value(&info, "package.version", identity->version) != CUP_OK ||
        require_info_value(&info, "platform.host", identity->host_platform) != CUP_OK ||
        require_info_value(&info, "platform.target", identity->target_platform) != CUP_OK) {
        info_free(&info);
        return CUP_ERR_VALIDATION;
    }

    err = validate_info_entries(&info, base_path);
    info_free(&info);
    return err;
}

CupError package_info_is_read_only(const char *base_path, int *is_read_only) {
    CupError err;
    char info_path[MAX_PATH_LEN];

    if (is_empty_string(base_path) || is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILE);
    if (err != CUP_OK) {
        return err;
    }

    return system_is_read_only(info_path, is_read_only);
}

CupError package_set_info_read_only(const char *base_path) {
    CupError err;
    char info_path[MAX_PATH_LEN];

    if (is_empty_string(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILE);
    if (err != CUP_OK) {
        return err;
    }

    return system_set_read_only(info_path, 1);
}

CupError package_exists(const PackageIdentity *identity, int *exists) {
    CupError err;
    char path[MAX_PATH_LEN];
    int is_directory;

    if (identity == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *exists = 0;

    err = layout_build_install_path(path, sizeof(path), identity);
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return err;
    }

    *exists = is_directory;
    return CUP_OK;
}

// COMPONENTS SCAN
typedef struct {
    PackageList *packages;
    PackageIdentity identity;
    int depth;
} PackageScanContext;

static CupError scan_package_path(const char *path, const SystemPathInfo *info, void *userdata) {
    PackageScanContext *context = userdata;
    PackageScanContext child;
    const char *name;
    CupError err;

    if (context == NULL || info == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (!info->is_directory || info->is_reparse_point) {
        context->packages->invalid_count++;
        return CUP_OK;
    }

    name = path_last_segment(path);
    if (name == NULL) {
        context->packages->invalid_count++;
        return CUP_OK;
    }

    child = *context;
    child.depth++;

    switch (context->depth) {
        case 0:
            if (validate_component(name) != CUP_OK) {
                context->packages->invalid_count++;
                return CUP_OK;
            }
            err = checked_snprintf(child.identity.component,
                sizeof(child.identity.component), "%s", name);
            break;

        case 1:
            if (validate_tool_for_component(context->identity.component, name) != CUP_OK) {
                context->packages->invalid_count++;
                return CUP_OK;
            }
            err = checked_snprintf(child.identity.tool,
                sizeof(child.identity.tool), "%s", name);
            break;

        case 2:
            if (validate_platform(name) != CUP_OK) {
                context->packages->invalid_count++;
                return CUP_OK;
            }
            err = checked_snprintf(child.identity.host_platform,
                sizeof(child.identity.host_platform), "%s", name);
            break;

        case 3:
            if (validate_platform(name) != CUP_OK) {
                context->packages->invalid_count++;
                return CUP_OK;
            }
            err = checked_snprintf(child.identity.target_platform,
                sizeof(child.identity.target_platform), "%s", name);
            break;

        case 4:
            if (!path_is_safe_identifier(name) ||
                context->packages->count >= MAX_SCANNED_PACKAGES) {
                context->packages->invalid_count++;
                return CUP_OK;
            }

            err = checked_snprintf(child.identity.version,
                sizeof(child.identity.version), "%s", name);
            if (err != CUP_OK || package_validate(path, &child.identity) != CUP_OK) {
                context->packages->invalid_count++;
                return CUP_OK;
            }

            context->packages->items[context->packages->count++] = child.identity;
            return CUP_OK;

        default:
            context->packages->invalid_count++;
            return CUP_OK;
    }

    if (err != CUP_OK) {
        context->packages->invalid_count++;
        return CUP_OK;
    }

    return system_list_directory(path, scan_package_path, &child);
}

CupError package_scan(PackageList *packages) {
    PackageScanContext context;
    CupError err;
    char root[MAX_PATH_LEN];
    int exists;

    if (packages == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(packages, 0, sizeof(*packages));
    memset(&context, 0, sizeof(context));
    context.packages = packages;

    err = layout_get_components_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = system_path_exists(root, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }

    return system_list_directory(root, scan_package_path, &context);
}
