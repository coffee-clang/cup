/*
 * Defines package identity validation, installed package inspection, component-tree scanning and
 * deterministic quarantine decisions.
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void package_diagnostic(FILE *diagnostics, const char *format, ...) {
    va_list args;

    if (diagnostics == NULL) {
        return;
    }
    va_start(args, format);
    vfprintf(diagnostics, format, args);
    va_end(args);
}

/* Concrete-release policy is shared with catalog validation; this layer only adds diagnostics. */
static CupError concrete_release_validate(const char *version, FILE *diagnostics) {
    CupError err = package_release_validate_concrete(version);

    if (err == CUP_ERR_INVALID_RELEASE) {
        package_diagnostic(diagnostics, "Error: invalid concrete package version '%s'.\n", version);
    }
    return err;
}

/* Package-scope and concrete-identity validation. */
static CupError package_identity_init_with_diagnostics(PackageIdentity *identity,
                                                       const char *component,
                                                       const char *tool,
                                                       const char *host_platform,
                                                       const char *target_platform,
                                                       const char *version,
                                                       FILE *diagnostics);
static CupError package_scope_init_with_diagnostics(PackageScope *scope,
                                                    const char *component,
                                                    const char *host_platform,
                                                    const char *target_platform,
                                                    FILE *diagnostics) {
    if (scope != NULL) {
        memset(scope, 0, sizeof(*scope));
    }
    if (scope == NULL || text_is_empty(component) || text_is_empty(host_platform) ||
        text_is_empty(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (!registry_is_component(component)) {
        package_diagnostic(diagnostics, "Error: unsupported component '%s'.\n", component);
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }
    if (!platform_is_supported(host_platform)) {
        package_diagnostic(diagnostics, "Error: unsupported platform '%s'.\n", host_platform);
        return CUP_ERR_INVALID_INPUT;
    }
    if (!platform_is_supported(target_platform)) {
        package_diagnostic(diagnostics, "Error: unsupported platform '%s'.\n", target_platform);
        return CUP_ERR_INVALID_INPUT;
    }
    if (text_copy(scope->component, sizeof(scope->component), component) != CUP_OK ||
        text_copy(scope->host_platform, sizeof(scope->host_platform), host_platform) != CUP_OK ||
        text_copy(scope->target_platform, sizeof(scope->target_platform), target_platform) !=
            CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform) {
    return package_scope_init_with_diagnostics(
        scope, component, host_platform, target_platform, stderr);
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

static int compare_identity_values(const void *left_value, const void *right_value) {
    const PackageIdentity *left = left_value;
    const PackageIdentity *right = right_value;
    int result;

    result = strcmp(left->host_platform, right->host_platform);
    if (result == 0) {
        result = strcmp(left->target_platform, right->target_platform);
    }
    if (result == 0) {
        result = strcmp(left->component, right->component);
    }
    if (result == 0) {
        result = strcmp(left->tool, right->tool);
    }
    if (result == 0) {
        result = strcmp(left->version, right->version);
    }
    return result;
}

int package_identity_matches(const PackageIdentity *identity,
                             const char *host_platform,
                             const char *target_platform,
                             const char *component) {
    if (identity == NULL || text_is_empty(host_platform)) {
        return 0;
    }

    return strcmp(identity->host_platform, host_platform) == 0 &&
           (target_platform == NULL ||
            strcmp(identity->target_platform, target_platform) == 0) &&
           (component == NULL || strcmp(identity->component, component) == 0);
}

void package_identity_sort(PackageIdentity *items, size_t count) {
    if (items != NULL && count > 1) {
        qsort(items, count, sizeof(items[0]), compare_identity_values);
    }
}

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    PackageIdentity validated;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    return package_identity_init_with_diagnostics(&validated,
                                                  identity->component,
                                                  identity->tool,
                                                  identity->host_platform,
                                                  identity->target_platform,
                                                  identity->version,
                                                  diagnostics);
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    CupError err;

    if (identity == NULL || buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_validate(identity, stderr);
    if (err != CUP_OK) {
        return err;
    }

    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

static CupError package_identity_init_with_diagnostics(PackageIdentity *identity,
                                                       const char *component,
                                                       const char *tool,
                                                       const char *host_platform,
                                                       const char *target_platform,
                                                       const char *version,
                                                       FILE *diagnostics) {
    PackageScope scope;
    CupError err;

    if (identity != NULL) {
        memset(identity, 0, sizeof(*identity));
    }
    if (identity == NULL || text_is_empty(tool) || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = package_scope_init_with_diagnostics(
        &scope, component, host_platform, target_platform, diagnostics);
    if (err != CUP_OK) {
        return err;
    }
    if (!registry_is_tool(scope.component, tool)) {
        package_diagnostic(diagnostics,
                           "Error: unsupported tool '%s' for component '%s'.\n",
                           tool,
                           scope.component);
        return CUP_ERR_INVALID_TOOL;
    }
    err = concrete_release_validate(version, diagnostics);
    if (err != CUP_OK) {
        return err;
    }
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

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    return package_identity_init_with_diagnostics(
        identity, component, tool, host_platform, target_platform, version, stderr);
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *selector_text,
                                        FILE *diagnostics) {
    PackageSelector selector;
    CupError err;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    err = package_selector_parse(&selector, selector_text);
    if (err != CUP_OK) {
        return err;
    }

    return package_identity_init_with_diagnostics(identity,
                                                  component,
                                                  selector.tool,
                                                  host_platform,
                                                  target_platform,
                                                  selector.release,
                                                  diagnostics);
}

/* Installed package validation. */
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
                                       const char *expected,
                                       FILE *diagnostics) {
    const char *actual;

    actual = package_metadata_get(metadata, key);
    if (actual == NULL || strcmp(actual, expected) != 0) {
        package_diagnostic(diagnostics,
                           "Error: package metadata field '%s' is missing or inconsistent.\n",
                           key);
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_package_commands(const PackageMetadata *metadata,
                                          const char *base_path,
                                          FILE *diagnostics) {
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
            package_diagnostic(diagnostics,
                               "Error: package command '%s' points to "
                               "invalid or non-executable file '%s'.\n",
                               command.name,
                               command.path);
            return CUP_ERR_VALIDATION;
        }

        count++;
    }

    if (count == 0) {
        package_diagnostic(diagnostics,
                           "Error: package metadata does not declare any external "
                           "entry.* command.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError load_validated_payload_metadata(PackageMetadata *metadata,
                                                const char *base_path,
                                                const PackageIdentity *identity,
                                                char *metadata_path,
                                                size_t metadata_path_size,
                                                FILE *diagnostics) {
    CupError err;

    if (metadata == NULL || text_is_empty(base_path) || identity == NULL ||
        metadata_path == NULL || metadata_path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = package_identity_validate(identity, diagnostics);
    if (err != CUP_OK) {
        return err;
    }
    err = path_join(metadata_path, metadata_path_size, base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    err = validate_nonempty_file(metadata_path);
    if (err != CUP_OK) {
        if (err != CUP_ERR_VALIDATION) {
            return err;
        }
        package_diagnostic(diagnostics, "Error: package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    err = package_metadata_load(metadata, metadata_path, diagnostics);
    if (err != CUP_OK) {
        return err == CUP_ERR_VALIDATION ? CUP_ERR_VALIDATION : err;
    }
    err = require_metadata_value(metadata, "package.component", identity->component, diagnostics);
    if (err == CUP_OK) {
        err = require_metadata_value(metadata, "package.tool", identity->tool, diagnostics);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(metadata, "package.version", identity->version, diagnostics);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(
            metadata, "platform.host", identity->host_platform, diagnostics);
    }
    if (err == CUP_OK) {
        err = require_metadata_value(
            metadata, "platform.target", identity->target_platform, diagnostics);
    }
    if (err == CUP_OK) {
        err = validate_package_commands(metadata, base_path, diagnostics);
    }
    return err;
}

void validated_package_init(ValidatedPackage *package) {
    if (package != NULL) {
        memset(package, 0, sizeof(*package));
        package_metadata_init(&package->metadata);
    }
}

void validated_package_free(ValidatedPackage *package) {
    if (package == NULL) {
        return;
    }
    package_metadata_free(&package->metadata);
    memset(package, 0, sizeof(*package));
}

CupError validated_package_load(ValidatedPackage *package,
                                const char *base_path,
                                const PackageIdentity *identity,
                                FILE *diagnostics) {
    PackageMetadata metadata;
    SystemPathIdentity root_identity;
    SystemPathIdentity current_root_identity;
    SystemPathIdentity current_metadata_identity;
    char metadata_path[MAX_PATH_LEN];
    CupError err;

    if (package == NULL || text_is_empty(base_path) || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    validated_package_free(package);
    package_metadata_init(&metadata);
    memset(&root_identity, 0, sizeof(root_identity));
    memset(&current_root_identity, 0, sizeof(current_root_identity));
    memset(&current_metadata_identity, 0, sizeof(current_metadata_identity));

    err = system_get_path_identity(base_path, &root_identity);
    if (err == CUP_OK &&
        (!root_identity.valid || root_identity.kind != SYSTEM_PATH_DIRECTORY)) {
        err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK) {
        err = load_validated_payload_metadata(
            &metadata, base_path, identity, metadata_path, sizeof(metadata_path), diagnostics);
    }
    if (err == CUP_OK) {
        err = system_get_path_identity(metadata_path, &current_metadata_identity);
    }
    if (err == CUP_OK &&
        !system_path_identity_equal(&metadata.identity, &current_metadata_identity)) {
        err = CUP_ERR_INCONSISTENT_STATE;
    }
    if (err == CUP_OK) {
        err = system_get_path_identity(base_path, &current_root_identity);
    }
    if (err == CUP_OK &&
        !system_path_identity_equal(&root_identity, &current_root_identity)) {
        err = CUP_ERR_INCONSISTENT_STATE;
    }
    if (err == CUP_OK) {
        package->metadata = metadata;
        package_metadata_init(&metadata);
    }

    package_metadata_free(&metadata);
    if (err != CUP_OK) {
        validated_package_free(package);
    }
    return err;
}

CupError package_validate(const char *base_path,
                          const PackageIdentity *identity,
                          FILE *diagnostics) {
    ValidatedPackage package;
    CupError err;

    validated_package_init(&package);
    err = validated_package_load(&package, base_path, identity, diagnostics);
    validated_package_free(&package);
    return err;
}

CupError package_metadata_is_read_only(const char *base_path, int *is_read_only) {
    char package_metadata_path[MAX_PATH_LEN];
    CupError err;

    if (is_read_only == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *is_read_only = 0;
    if (text_is_empty(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    return system_is_read_only(package_metadata_path, is_read_only);
}

CupError package_set_metadata_read_only(const char *base_path) {
    char package_metadata_path[MAX_PATH_LEN];
    int metadata_read_only;
    CupError err;

    if (text_is_empty(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(
        package_metadata_path, sizeof(package_metadata_path), base_path, CUP_INFO_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    err = system_is_read_only(package_metadata_path, &metadata_read_only);
    if (err != CUP_OK || metadata_read_only) {
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
    FILE *diagnostics;
} PackageScanContext;

int package_list_contains(const PackageList *packages, const PackageIdentity *package) {
    size_t i;

    if (packages == NULL || package == NULL || packages->count > MAX_SCANNED_PACKAGES) {
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
                              const PackageIdentity *package,
                              const SystemPathIdentity *path_identity) {
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
        if (path_identity != NULL) {
            issue->path_identity = *path_identity;
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
                                  const SystemPathIdentity *path_identity,
                                  const char *name) {
    PackageIdentity package = context->identity;
    CupError err;

    if (package_release_validate_concrete(name) != CUP_OK ||
        text_copy(package.version, sizeof(package.version), name) != CUP_OK) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_VERSION, 0, NULL, NULL);
        return CUP_OK;
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        int can_quarantine = path_kind == SYSTEM_PATH_REGULAR_FILE;

        record_scan_issue(context,
                          path,
                          PACKAGE_ISSUE_INVALID_PATH_TYPE,
                          can_quarantine,
                          &package,
                          can_quarantine ? path_identity : NULL);
        return CUP_OK;
    }

    err = package_validate(path, &package, context->diagnostics);
    if (err == CUP_ERR_VALIDATION) {
        record_scan_issue(
            context, path, PACKAGE_ISSUE_INVALID_CONTENT, 1, &package, path_identity);
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }

    record_valid_package(context->packages, &package);
    return CUP_OK;
}

static CupError scan_package_path(const char *path,
                                  SystemPathKind path_kind,
                                  const SystemPathIdentity *identity,
                                  void *userdata) {
    PackageScanContext *context = userdata;
    PackageScanContext child;
    const char *name;
    CupError err = CUP_OK;

    if (context == NULL || identity == NULL || !identity->valid || identity->kind != path_kind) {
        return CUP_ERR_INVALID_INPUT;
    }

    name = path_last_segment(path);
    if (name == NULL) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL, NULL);
        return CUP_OK;
    }

    /* Version directories are leaves; every earlier level must be a directory. */
    if (context->level == PACKAGE_LEVEL_VERSION) {
        return scan_version_path(context, path, path_kind, identity, name);
    }

    if (path_kind != SYSTEM_PATH_DIRECTORY) {
        record_scan_issue(context, path, PACKAGE_ISSUE_INVALID_PATH_TYPE, 0, NULL, NULL);
        return CUP_OK;
    }

    child = *context;
    child.level = (PackagePathLevel)(context->level + 1);

    /* Validate and copy exactly the identity field owned by the current level. */
    switch (context->level) {
        case PACKAGE_LEVEL_COMPONENT:
            if (registry_is_component(name)) {
                err = text_copy(child.identity.component, sizeof(child.identity.component), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_TOOL:
            if (registry_is_tool(context->identity.component, name)) {
                err = text_copy(child.identity.tool, sizeof(child.identity.tool), name);
            } else {
                err = CUP_ERR_VALIDATION;
            }
            break;

        case PACKAGE_LEVEL_HOST:
            if (platform_is_supported(name)) {
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
            if (platform_is_supported(name)) {
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
        record_scan_issue(context, path, invalid_name_reason(context->level), 0, NULL, NULL);
        return CUP_OK;
    }

    /* Descend only after the current segment has produced a valid child identity. */
    return system_list_directory(path, scan_package_path, &child);
}

CupError package_scan(PackageList *packages, FILE *diagnostics) {
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
    context.diagnostics = diagnostics;
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
        text_is_empty(issue->path) || !issue->path_identity.valid ||
        (issue->path_identity.kind != SYSTEM_PATH_REGULAR_FILE &&
         issue->path_identity.kind != SYSTEM_PATH_DIRECTORY) ||
        package_identity_validate(&issue->package, NULL) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_create_recovery_dir(recovery_dir, sizeof(recovery_dir), &issue->package);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(recovery_path, recovery_size, recovery_dir, "package");
    if (err != CUP_OK) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK ? err : CUP_ERR_ROLLBACK;
    }

    err = system_move_path_if_identity(
        issue->path, recovery_path, &issue->path_identity, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        return filesystem_remove_tree(recovery_dir) == CUP_OK ? err : CUP_ERR_ROLLBACK;
    }

    return CUP_ERR_COMMIT;
}
