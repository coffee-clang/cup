/*
 * Defines package identity validation, installed package inspection, component-tree scanning and
 * deterministic quarantine eligibility.
 */

#include "package.h"

#include "package_selector.h"
#include "filesystem.h"
#include "package_metadata.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Package scope and identity. */
CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform) {
    CupError err;

    if (scope == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = registry_validate_component(component);
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

    memset(scope, 0, sizeof(*scope));
    if (text_copy(scope->component, sizeof(scope->component), component) != CUP_OK ||
        text_copy(scope->host_platform, sizeof(scope->host_platform), host_platform) != CUP_OK ||
        text_copy(scope->target_platform, sizeof(scope->target_platform), target_platform) !=
            CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

int package_scope_equals(const PackageScope *left, const PackageScope *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0;
}

CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope) {
    if (identity == NULL || scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(scope, 0, sizeof(*scope));
    if (text_copy(scope->component, sizeof(scope->component), identity->component) != CUP_OK ||
        text_copy(scope->host_platform, sizeof(scope->host_platform), identity->host_platform) !=
            CUP_OK ||
        text_copy(scope->target_platform,
                  sizeof(scope->target_platform),
                  identity->target_platform) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

CupError package_identity_validate(const PackageIdentity *identity) {
    PackageIdentity validated;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    return package_identity_init(&validated,
                                 identity->component,
                                 identity->tool,
                                 identity->host_platform,
                                 identity->target_platform,
                                 identity->version);
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    CupError err;

    if (identity == NULL || buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_validate(identity);
    if (err != CUP_OK) {
        return err;
    }

    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    PackageScope scope;
    CupError err;

    if (identity == NULL || text_is_empty(tool) || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_scope_init(&scope, component, host_platform, target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = registry_validate_tool(scope.component, tool);
    if (err != CUP_OK) {
        return err;
    }

    if (package_release_is_stable(version) || !path_is_safe_identifier(version)) {
        fprintf(stderr, "Error: invalid concrete package version '%s'.\n", version);
        return CUP_ERR_INVALID_RELEASE;
    }

    memset(identity, 0, sizeof(*identity));

    if (text_copy(identity->component, sizeof(identity->component), scope.component) != CUP_OK ||
        text_copy(identity->tool, sizeof(identity->tool), tool) != CUP_OK ||
        text_copy(identity->host_platform, sizeof(identity->host_platform), scope.host_platform) !=
            CUP_OK ||
        text_copy(identity->target_platform,
                  sizeof(identity->target_platform),
                  scope.target_platform) != CUP_OK ||
        text_copy(identity->version, sizeof(identity->version), version) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *selector_text) {
    PackageSelector selector;
    CupError err;

    err = package_selector_parse(&selector, selector_text);
    if (err != CUP_OK) {
        return err;
    }

    return package_identity_init(
        identity, component, selector.tool, host_platform, target_platform, selector.release);
}

/* Installed package validation. */
static CupError validate_directory(const char *path) {
    CupError err;
    int is_directory;

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_directory) {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_nonempty_file(const char *path) {
    CupError err;
    long long size;
    int is_regular_file;

    err = system_is_regular_file(path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular_file) {
        return CUP_ERR_VALIDATION;
    }

    err = system_file_size(path, &size);
    if (err != CUP_OK) {
        return err;
    }
    if (size <= 0) {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError require_metadata_value(const PackageMetadata *metadata,
                                       const char *key,
                                       const char *expected) {
    const char *actual;

    actual = package_metadata_get(metadata, key);
    if (actual == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr, "Error: package metadata field '%s' is missing or inconsistent.\n", key);
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_package_commands(const PackageMetadata *metadata, const char *base_path) {
    PackageCommand command;
    size_t cursor = 0;
    size_t count = 0;

    while (package_metadata_next_command(metadata, &command, &cursor)) {
        char path[MAX_PATH_LEN];
        CupError err;
        int is_executable = 0;

        if (!path_is_safe_relative(command.path)) {
            err = CUP_ERR_VALIDATION;
        } else {
            err = path_join_safe_relative(path, sizeof(path), base_path, command.path);
        }
        if (err == CUP_OK) {
            err = validate_nonempty_file(path);
        }
        if (err == CUP_OK) {
            err = system_is_executable(path, &is_executable);
        }
        if (err == CUP_OK && !is_executable) {
            err = CUP_ERR_VALIDATION;
        }

        if (err != CUP_OK) {
            if (err != CUP_ERR_VALIDATION) {
                return err;
            }
            fprintf(stderr,
                    "Error: package command '%s' points to "
                    "invalid or non-executable file '%s'.\n",
                    command.name,
                    command.path);
            return CUP_ERR_VALIDATION;
        }

        count++;
    }

    if (count == 0) {
        fprintf(stderr, "Error: package metadata does not declare any external entry.* command.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    PackageMetadata metadata;
    CupError err;
    char package_metadata_path[MAX_PATH_LEN];

    if (text_is_empty(base_path) || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_directory(base_path);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_nonempty_file(package_metadata_path);
    if (err != CUP_OK) {
        if (err != CUP_ERR_VALIDATION) {
            return err;
        }
        fprintf(stderr, "Error: package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    package_metadata_init(&metadata);
    err = package_metadata_load(&metadata, package_metadata_path);
    if (err != CUP_OK) {
        package_metadata_free(&metadata);
        return err == CUP_ERR_VALIDATION ? CUP_ERR_VALIDATION : err;
    }

    err = require_metadata_value(&metadata, "package.component", identity->component);
    if (err == CUP_OK) {
        err = require_metadata_value(&metadata, "package.tool", identity->tool);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(&metadata, "package.version", identity->version);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(&metadata, "platform.host", identity->host_platform);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(&metadata, "platform.target", identity->target_platform);
    }
    if (err != CUP_OK) {
        package_metadata_free(&metadata);
        return err;
    }

    err = validate_package_commands(&metadata, base_path);
    package_metadata_free(&metadata);
    return err;
}

CupError package_metadata_is_read_only(const char *base_path, int *is_read_only) {
    CupError err;
    char package_metadata_path[MAX_PATH_LEN];

    if (text_is_empty(base_path) || is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = 0;

    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }

    return system_is_read_only(package_metadata_path, is_read_only);
}

CupError package_set_metadata_read_only(const char *base_path) {
    CupError err;
    char package_metadata_path[MAX_PATH_LEN];

    if (text_is_empty(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }

    return system_set_read_only(package_metadata_path, 1);
}

CupError package_path_exists(const PackageIdentity *identity, int *exists) {
    CupError err;
    char path[MAX_PATH_LEN];

    if (identity == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *exists = 0;

    err = layout_build_install_path(path, sizeof(path), identity);
    if (err != CUP_OK) {
        return err;
    }

    return system_path_exists(path, exists);
}

/* Canonical component-tree scan and quarantine metadata. */
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
    char current_host[MAX_PLATFORM_LEN];
    PackagePathLevel level;
} PackageScanContext;

int package_list_contains(const PackageList *packages, const PackageIdentity *package) {
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

static void record_scan_issue(PackageScanContext *context,
                              const char *path,
                              PackageIssueReason reason,
                              int can_quarantine,
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
        if (text_copy(issue->path, sizeof(issue->path), path) != CUP_OK) {
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

static void record_valid_package(PackageList *packages, const PackageIdentity *package) {
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
                                  const char *path,
                                  SystemPathKind path_kind,
                                  const char *name) {
    PackageIdentity package = context->identity;
    CupError err;

    if (!path_is_safe_identifier(name) ||
        text_copy(package.version, sizeof(package.version), name) != CUP_OK) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_VERSION, 0, NULL);
        return CUP_OK;
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_PATH_TYPE, 1, &package);
        return CUP_OK;
    }

    err = package_validate(path, &package);
    if (err == CUP_ERR_VALIDATION) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_CONTENT, 1, &package);
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }

    record_valid_package(context->packages, &package);
    return CUP_OK;
}

static CupError scan_package_path(const char *path, SystemPathKind path_kind, void *userdata) {
    PackageScanContext *context = userdata;
    PackageScanContext child;
    const char *name;
    CupError err = CUP_OK;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL);
        return CUP_OK;
    }

    /* Version directories are leaves; every earlier level must be a directory. */
    if (context->level == PACKAGE_LEVEL_VERSION) {
        return scan_version_path(context, path, path_kind, name);
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL);
        return CUP_OK;
    }

    child = *context;
    child.level = (PackagePathLevel)(context->level + 1);

    /* Validate and copy exactly the identity field owned by the current level. */
    switch (context->level) {
        case PACKAGE_LEVEL_COMPONENT:
            if (registry_validate_component(name) == CUP_OK) {
                err = text_copy(child.identity.component, sizeof(child.identity.component), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_TOOL:
            if (registry_validate_tool(context->identity.component, name) == CUP_OK) {
                err = text_copy(child.identity.tool, sizeof(child.identity.tool), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_HOST:
            if (platform_validate(name) == CUP_OK) {
                if (strcmp(name, context->current_host) != 0) {
                    context->packages->foreign_host_count++;
                    return CUP_OK;
                }
                err = text_copy(
                    child.identity.host_platform, sizeof(child.identity.host_platform), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_TARGET:
            if (platform_validate(name) == CUP_OK) {
                err = text_copy(
                    child.identity.target_platform, sizeof(child.identity.target_platform), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_VERSION:
            /* Version entries are handled before descending further. */
            err = CUP_ERR_VALIDATION;
            break;

        default:
            err = CUP_ERR_VALIDATION;
            break;
    }

    if (err != CUP_OK) {
        record_scan_issue(context, path, invalid_name_reason(context->level), 0, NULL);
        return CUP_OK;
    }

    /* Descend only after the current segment has produced a valid child identity. */
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
    err = platform_get_host(context.current_host, sizeof(context.current_host));
    if (err != CUP_OK) {
        return err;
    }

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

CupError package_quarantine(const PackageIssue *issue, char *recovery_path, size_t recovery_size) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char recovery_dir[MAX_PATH_LEN];

    if (issue == NULL || recovery_path == NULL || recovery_size == 0 || !issue->can_quarantine ||
        text_is_empty(issue->path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_create_recovery_dir(recovery_dir, sizeof(recovery_dir), &issue->package);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(recovery_path, recovery_size, recovery_dir, "package");
    if (err != CUP_OK) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK ? err : CUP_ERR_TEMPORARY;
    }

    err = system_move_path(issue->path, recovery_path, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK ? err : CUP_ERR_TEMPORARY;
    }

    return CUP_ERR_COMMIT;
}
