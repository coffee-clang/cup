#include "package.h"

#include "entry.h"
#include "filesystem.h"
#include "info.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

// PACKAGE IDENTITY
CupError package_identity_init(PackageIdentity *identity, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *version) {
    CupError err;

    if (identity == NULL || text_is_empty(component) || text_is_empty(tool) ||
        text_is_empty(host_platform) || text_is_empty(target_platform) ||
        text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = registry_validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = registry_validate_tool(component, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = platform_validate(host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = platform_validate(target_platform);
    if (err != CUP_OK) {
        return err;
    }

    if (!path_is_safe_identifier(version)) {
        fprintf(stderr, "Error: invalid package version '%s'.\n", version);
        return CUP_ERR_INVALID_RELEASE;
    }

    memset(identity, 0, sizeof(*identity));

    if (text_format(identity->component, sizeof(identity->component), "%s", component) != CUP_OK ||
        text_format(identity->tool, sizeof(identity->tool), "%s", tool) != CUP_OK ||
        text_format(identity->host_platform,
            sizeof(identity->host_platform), "%s", host_platform) != CUP_OK ||
        text_format(identity->target_platform, sizeof(identity->target_platform),
            "%s", target_platform) != CUP_OK ||
        text_format(identity->version, sizeof(identity->version), "%s", version) != CUP_OK) {
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
static CupError validate_directory(const char *path) {
    int is_directory;

    if (system_is_directory(path, &is_directory) != CUP_OK || !is_directory) {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_nonempty_file(const char *path) {
    long long size;
    int is_regular_file;

    if (system_is_regular_file(path, &is_regular_file) != CUP_OK ||
        !is_regular_file) {
        return CUP_ERR_VALIDATION;
    }

    if (system_file_size(path, &size) != CUP_OK || size <= 0) {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
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
        int is_executable;

        if (!path_is_safe_relative(field->value) ||
            path_join_safe_relative(path, sizeof(path), base_path, field->value) != CUP_OK ||
            validate_nonempty_file(path) != CUP_OK ||
            system_is_executable(path, &is_executable) != CUP_OK || !is_executable) {
            fprintf(stderr, "Error: package metadata entry '%s' points to "
                "invalid or non-executable file '%s'.\n",
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

    if (text_is_empty(base_path) || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_directory(base_path);
    if (err != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK || validate_nonempty_file(info_path) != CUP_OK) {
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

    if (text_is_empty(base_path) || is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }

    return system_is_read_only(info_path, is_read_only);
}

CupError package_set_info_read_only(const char *base_path) {
    CupError err;
    char info_path[MAX_PATH_LEN];

    if (text_is_empty(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(info_path, sizeof(info_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }

    return system_set_read_only(info_path, 1);
}

CupError package_path_exists(const PackageIdentity *identity, int *exists) {
    CupError err;
    char path[MAX_PATH_LEN];

    if (identity == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_build_install_path(path, sizeof(path), identity);
    if (err != CUP_OK) {
        return err;
    }

    return system_path_exists(path, exists);
}

// COMPONENTS SCAN
typedef enum {
    PACKAGE_LEVEL_COMPONENT,
    PACKAGE_LEVEL_TOOL,
    PACKAGE_LEVEL_HOST,
    PACKAGE_LEVEL_TARGET,
    PACKAGE_LEVEL_VERSION
} PackagePathLevel;

typedef struct {
    PackageList *packages;
    PackageIdentity identity;
    PackagePathLevel level;
} PackageScanContext;

static int package_identity_equals(const PackageIdentity *left,
    const PackageIdentity *right) {
    return strcmp(left->component, right->component) == 0 &&
        strcmp(left->tool, right->tool) == 0 &&
        strcmp(left->host_platform, right->host_platform) == 0 &&
        strcmp(left->target_platform, right->target_platform) == 0 &&
        strcmp(left->version, right->version) == 0;
}

int package_list_contains(const PackageList *packages,
    const PackageIdentity *package) {
    size_t i;

    if (packages == NULL || package == NULL) {
        return 0;
    }

    for (i = 0; i < packages->count; ++i) {
        if (package_identity_equals(&packages->items[i], package)) {
            return 1;
        }
    }

    return 0;
}

const char *package_issue_reason_name(PackageIssueReason reason) {
    switch (reason) {
        case PACKAGE_ISSUE_INVALID_PATH_TYPE:
            return "unexpected path type";
        case PACKAGE_ISSUE_INVALID_COMPONENT:
            return "unknown component";
        case PACKAGE_ISSUE_INVALID_TOOL:
            return "unknown tool";
        case PACKAGE_ISSUE_INVALID_HOST:
            return "invalid host platform";
        case PACKAGE_ISSUE_INVALID_TARGET:
            return "invalid target platform";
        case PACKAGE_ISSUE_INVALID_VERSION:
            return "invalid package version";
        case PACKAGE_ISSUE_INVALID_CONTENT:
            return "invalid package contents";
        default:
            return "unknown package issue";
    }
}

static void record_scan_issue(PackageScanContext *context, const char *path,
    PackageIssueReason reason, int can_quarantine,
    const PackageIdentity *package) {
    PackageList *packages = context->packages;

    packages->total_issue_count++;
    if (packages->issue_count >= MAX_PACKAGE_SCAN_ISSUES) {
        packages->complete = 0;
        return;
    }

    {
        PackageIssue *issue = &packages->issues[packages->issue_count++];

        memset(issue, 0, sizeof(*issue));
        if (text_format(issue->path, sizeof(issue->path), "%s", path) != CUP_OK) {
            packages->complete = 0;
            packages->issue_count--;
            return;
        }

        issue->reason = reason;
        issue->can_quarantine = can_quarantine;
        if (package != NULL) {
            issue->package = *package;
        }
    }
}

static void record_valid_package(PackageList *packages,
    const PackageIdentity *package) {
    packages->total_count++;
    if (packages->count >= MAX_SCANNED_PACKAGES) {
        packages->complete = 0;
        return;
    }

    packages->items[packages->count++] = *package;
}

static PackageIssueReason invalid_name_reason(PackagePathLevel level) {
    switch (level) {
        case PACKAGE_LEVEL_COMPONENT:
            return PACKAGE_ISSUE_INVALID_COMPONENT;
        case PACKAGE_LEVEL_TOOL:
            return PACKAGE_ISSUE_INVALID_TOOL;
        case PACKAGE_LEVEL_HOST:
            return PACKAGE_ISSUE_INVALID_HOST;
        case PACKAGE_LEVEL_TARGET:
            return PACKAGE_ISSUE_INVALID_TARGET;
        case PACKAGE_LEVEL_VERSION:
            return PACKAGE_ISSUE_INVALID_VERSION;
        default:
            return PACKAGE_ISSUE_INVALID_PATH_TYPE;
    }
}

static CupError scan_version_path(PackageScanContext *context,
    const char *path, SystemPathKind path_kind, const char *name) {
    PackageIdentity package = context->identity;

    if (!path_is_safe_identifier(name) ||
        text_format(package.version, sizeof(package.version), "%s", name) != CUP_OK) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_VERSION, 0, NULL);
        return CUP_OK;
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY ||
        package_validate(path, &package) != CUP_OK) {
        record_scan_issue(context, path,
            path_kind == SYSTEM_PATH_DIRECTORY
                ? PACKAGE_ISSUE_INVALID_CONTENT
                : PACKAGE_ISSUE_INVALID_PATH_TYPE,
            1, &package);
        return CUP_OK;
    }

    record_valid_package(context->packages, &package);
    return CUP_OK;
}

static CupError scan_package_path(const char *path,
    SystemPathKind path_kind, void *userdata) {
    PackageScanContext *context = userdata;
    PackageScanContext child;
    const char *name;
    CupError err = CUP_OK;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL) {
        record_scan_issue(context, path,
            PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL);
        return CUP_OK;
    }

    if (context->level == PACKAGE_LEVEL_VERSION) {
        return scan_version_path(context, path, path_kind, name);
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        record_scan_issue(context, path,
            PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL);
        return CUP_OK;
    }

    child = *context;
    child.level = (PackagePathLevel)(context->level + 1);

    switch (context->level) {
        case PACKAGE_LEVEL_COMPONENT:
            if (registry_validate_component(name) == CUP_OK) {
                err = text_format(child.identity.component,
                    sizeof(child.identity.component), "%s", name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_TOOL:
            if (registry_validate_tool(context->identity.component, name) == CUP_OK) {
                err = text_format(child.identity.tool,
                    sizeof(child.identity.tool), "%s", name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_HOST:
            if (platform_validate(name) == CUP_OK) {
                err = text_format(child.identity.host_platform,
                    sizeof(child.identity.host_platform), "%s", name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_TARGET:
            if (platform_validate(name) == CUP_OK) {
                err = text_format(child.identity.target_platform,
                    sizeof(child.identity.target_platform), "%s", name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        default:
            err = CUP_ERR_VALIDATION;
            break;
    }

    if (err != CUP_OK) {
        record_scan_issue(context, path,
            invalid_name_reason(context->level), 0, NULL);
        return CUP_OK;
    }

    return system_list_directory(path, scan_package_path, &child);
}

CupError package_scan(PackageList *packages) {
    PackageScanContext context;
    CupError err;
    char root[MAX_PATH_LEN];
    SystemPathKind root_kind;

    if (packages == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(packages, 0, sizeof(*packages));
    packages->complete = 1;
    memset(&context, 0, sizeof(context));
    context.packages = packages;

    err = layout_get_components_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = system_get_path_kind(root, &root_kind);
    if (err != CUP_OK) {
        return err;
    }
    if (root_kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (root_kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_FILESYSTEM;
    }

    return system_list_directory(root, scan_package_path, &context);
}

CupError package_quarantine(const PackageIssue *issue,
    char *recovery_path, size_t recovery_size) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char recovery_dir[MAX_PATH_LEN];

    if (issue == NULL || recovery_path == NULL || recovery_size == 0 ||
        !issue->can_quarantine || text_is_empty(issue->path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_create_recovery_dir(recovery_dir,
        sizeof(recovery_dir), &issue->package);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(recovery_path, recovery_size,
        recovery_dir, "package");
    if (err != CUP_OK) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK
            ? err : CUP_ERR_TEMPORARY;
    }

    err = system_move_path(issue->path, recovery_path, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK
            ? err : CUP_ERR_TEMPORARY;
    }

    return CUP_ERR_COMMIT;
}
