/*
 * Test focus: Exercises package identities, metadata validation, executable entries, scanning
 * and quarantine decisions.
 */

#include "error.h"
#include "package.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "unity.h"

#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[] = "/tmp/cup-package-test-XXXXXX";
static unsigned int recovery_serial;
static CupError install_path_result;
static CupError components_path_result;
static CupError recovery_result;
static CupError cleanup_result;
static int cleanup_calls;

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    int written;

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (install_path_result != CUP_OK) {
        return install_path_result;
    }
    written =
        snprintf(buffer, size, "%s/install/%s-%s", temp_dir, identity->tool, identity->version);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_components_dir(char *buffer, size_t size) {
    int written;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (components_path_result != CUP_OK) {
        return components_path_result;
    }
    written = snprintf(buffer, size, "%s/components", temp_dir);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_create_recovery_dir(char *buffer, size_t size, const PackageIdentity *identity) {
    int written;

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (recovery_result != CUP_OK) {
        return recovery_result;
    }
    written = snprintf(buffer, size, "%s/recovery-%u", temp_dir, recovery_serial++);
    if (written < 0 || (size_t)written >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (mkdir(buffer, 0755) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

CupError filesystem_remove_tree(const char *path) {
    (void)path;
    cleanup_calls++;
    return cleanup_result;
}

void setUp(void) {
    install_path_result = CUP_OK;
    components_path_result = CUP_OK;
    recovery_result = CUP_OK;
    cleanup_result = CUP_OK;
    cleanup_calls = 0;
}
void tearDown(void) {
}

static void build_path(char *out, size_t size, const char *name) {
    int written = snprintf(out, size, "%s/%s", temp_dir, name);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void join_path(char *out, size_t size, const char *left, const char *right) {
    int written = snprintf(out, size, "%s/%s", left, right);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(mkdir(path, 0755) == 0 || access(path, F_OK) == 0);
}

static void make_parent_chain(const char *relative) {
    char current[512];
    char copy[512];
    char *saveptr = NULL;
    char *segment;

    TEST_ASSERT_TRUE(snprintf(copy, sizeof(copy), "%s", relative) > 0);
    TEST_ASSERT_TRUE(snprintf(current, sizeof(current), "%s", temp_dir) > 0);

    segment = strtok_r(copy, "/", &saveptr);
    while (segment != NULL) {
        char next[512];

        if (*segment == '\0') {
            continue;
        }
        join_path(next, sizeof(next), current, segment);
        make_dir(next);
        TEST_ASSERT_TRUE(snprintf(current, sizeof(current), "%s", next) > 0);
        segment = strtok_r(NULL, "/", &saveptr);
    }
}

static void make_valid_package(const char *root) {
    char bin_dir[512];
    char tool_path[512];
    char package_metadata_path[512];

    join_path(bin_dir, sizeof(bin_dir), root, "bin");
    join_path(tool_path, sizeof(tool_path), bin_dir, "clang");
    join_path(package_metadata_path, sizeof(package_metadata_path), root, "info.txt");

    make_dir(root);
    make_dir(bin_dir);
    write_text(tool_path, "#!/bin/sh\nexit 0\n");
    TEST_ASSERT_EQUAL_INT(0, chmod(tool_path, 0755));
    write_text(package_metadata_path,
               "package.component=compiler\n"
               "package.tool=clang\n"
               "package.version=22.1.5\n"
               "platform.host=linux-x64\n"
               "platform.target=linux-x64\n"
               "entry.clang=bin/clang\n");
}

static const PackageIssue *find_issue(const PackageList *packages,
                                      PackageIssueReason reason,
                                      int can_quarantine) {
    size_t i;

    for (i = 0; i < packages->issue_count; ++i) {
        if (packages->issues[i].reason == reason &&
            packages->issues[i].can_quarantine == can_quarantine) {
            return &packages->issues[i];
        }
    }
    return NULL;
}

static void test_scope_validation(void) {
    PackageScope first;
    PackageScope same;
    PackageScope different;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_scope_init(&first, "compiler", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_STRING("compiler", first.component);
    TEST_ASSERT_EQUAL_STRING("linux-x64", first.host_platform);
    TEST_ASSERT_EQUAL_STRING("linux-x64", first.target_platform);

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_scope_init(&same, "compiler", "linux-x64", "linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_scope_init(&different, "compiler", "linux-x64", "windows-x64"));
    TEST_ASSERT_TRUE(package_scope_equals(&first, &same));
    TEST_ASSERT_FALSE(package_scope_equals(&first, &different));
    TEST_ASSERT_FALSE(package_scope_equals(NULL, &same));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_scope_init(NULL, "compiler", "linux-x64", "linux-x64"));
    TEST_ASSERT_NOT_EQUAL(CUP_OK, package_scope_init(&first, "unknown", "linux-x64", "linux-x64"));
    TEST_ASSERT_NOT_EQUAL(CUP_OK,
                          package_scope_init(&first, "compiler", "bad-platform", "linux-x64"));
}

static void test_identity_validation(void) {
    PackageIdentity identity;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_EQUAL_STRING("compiler", identity.component);
    TEST_ASSERT_EQUAL_STRING("clang", identity.tool);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_identity_from_selector(
                              &identity, "compiler", "linux-x64", "linux-x64", "clang@22.1.5"));
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity.version);

    {
        PackageIdentity same;
        PackageIdentity different;
        PackageScope scope;

        TEST_ASSERT_EQUAL_INT(
            CUP_OK,
            package_identity_init(&same, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
        TEST_ASSERT_EQUAL_INT(
            CUP_OK,
            package_identity_init(
                &different, "compiler", "clang", "linux-x64", "linux-x64", "22.1.6"));
        TEST_ASSERT_TRUE(package_identity_equals(&identity, &same));
        TEST_ASSERT_FALSE(package_identity_equals(&identity, &different));
        TEST_ASSERT_FALSE(package_identity_equals(NULL, &same));
        TEST_ASSERT_EQUAL_INT(CUP_OK, package_identity_get_scope(&identity, &scope));
        TEST_ASSERT_EQUAL_STRING("compiler", scope.component);
        TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_identity_get_scope(NULL, &scope));
    }

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_identity_init(NULL, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_NOT_EQUAL(
        CUP_OK,
        package_identity_init(&identity, "unknown", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_NOT_EQUAL(
        CUP_OK,
        package_identity_init(
            &identity, "compiler", "clang", "linux-x64", "bad-platform", "22.1.5"));
    TEST_ASSERT_NOT_EQUAL(
        CUP_OK,
        package_identity_init(
            &identity, "compiler", "clang", "linux-x64", "linux-x64", "../escape"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_RELEASE,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "stable"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_identity_from_selector(
                              &identity, "compiler", "linux-x64", "linux-x64", "clang@stable"));
    TEST_ASSERT_NOT_EQUAL(
        CUP_OK,
        package_identity_from_selector(&identity, "compiler", "linux-x64", "linux-x64", "clang"));
}

static void test_valid_package(void) {
    PackageIdentity identity;
    char root[512];

    build_path(root, sizeof(root), "valid-package");
    make_valid_package(root);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_validate(root, &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_validate(NULL, &identity));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_validate(root, NULL));
}

static void test_invalid_package(void) {
    PackageIdentity identity;
    char root[512];
    char path[512];

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));

    build_path(root, sizeof(root), "not-a-directory");
    write_text(root, "file\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "missing-info");
    make_dir(root);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "missing-entry");
    make_valid_package(root);
    join_path(path, sizeof(path), root, "bin/clang");
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "metadata-mismatch");
    make_valid_package(root);
    join_path(path, sizeof(path), root, "info.txt");
    write_text(path,
               "package.component=compiler\n"
               "package.tool=clang\n"
               "package.version=21.1.5\n"
               "platform.host=linux-x64\n"
               "platform.target=linux-x64\n"
               "entry.clang=bin/clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "no-declared-entry");
    make_valid_package(root);
    join_path(path, sizeof(path), root, "info.txt");
    write_text(path,
               "package.component=compiler\n"
               "package.tool=clang\n"
               "package.version=22.1.5\n"
               "platform.host=linux-x64\n"
               "platform.target=linux-x64\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "unsafe-entry");
    make_valid_package(root);
    join_path(path, sizeof(path), root, "info.txt");
    write_text(path,
               "package.component=compiler\n"
               "package.tool=clang\n"
               "package.version=22.1.5\n"
               "platform.host=linux-x64\n"
               "platform.target=linux-x64\n"
               "entry.clang=../clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));

    build_path(root, sizeof(root), "non-exec");
    make_valid_package(root);
    join_path(path, sizeof(path), root, "bin/clang");
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0644));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, package_validate(root, &identity));
}

static void test_metadata_paths(void) {
    PackageIdentity identity;
    char root[512];
    char package_metadata_path[512];
    char install_path[512];
    int value;

    build_path(root, sizeof(root), "permission-package");
    make_valid_package(root);
    join_path(package_metadata_path, sizeof(package_metadata_path), root, "info.txt");

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_metadata_is_read_only(root, &value));
    TEST_ASSERT_FALSE(value);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_set_metadata_read_only(root));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_metadata_is_read_only(root, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_metadata_is_read_only(NULL, &value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_metadata_is_read_only(root, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_set_metadata_read_only(NULL));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          layout_build_install_path(install_path, sizeof(install_path), &identity));
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_path_exists(&identity, &value));
    TEST_ASSERT_FALSE(value);
    make_parent_chain("install");
    make_dir(install_path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_path_exists(&identity, &value));
    TEST_ASSERT_TRUE(value);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_path_exists(NULL, &value));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_path_exists(&identity, NULL));
}

static void test_scan_roots(void) {
    PackageList packages;
    char components[512];

    build_path(components, sizeof(components), "components");
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_scan(&packages));
    TEST_ASSERT_TRUE(packages.complete);
    TEST_ASSERT_EQUAL_size_t(0, packages.count);

    write_text(components, "not a directory\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, package_scan(&packages));
    TEST_ASSERT_EQUAL_INT(0, unlink(components));

    components_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, package_scan(&packages));
}

static void test_path_failures(void) {
    PackageIdentity identity;
    PackageIssue issue;
    char recovery[512];
    int exists;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, package_path_exists(&identity, &exists));

    memset(&issue, 0, sizeof(issue));
    issue.can_quarantine = 1;
    issue.package = identity;
    build_path(issue.path, sizeof(issue.path), "missing-package");

    recovery_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY,
                          package_quarantine(&issue, recovery, sizeof(recovery)));

    recovery_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, package_quarantine(&issue, recovery, 1));
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    cleanup_calls = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_quarantine(&issue, recovery, sizeof(recovery)));
    TEST_ASSERT_EQUAL_INT(1, cleanup_calls);

    cleanup_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY,
                          package_quarantine(&issue, recovery, sizeof(recovery)));
}

static void test_scan_issues(void) {
    PackageIdentity identity;
    PackageList packages;
    const PackageIssue *invalid_content;
    char components[512];
    char valid_root[512];
    char path[512];
    char recovery_path[512];
    int exists;

    build_path(components, sizeof(components), "components");
    make_parent_chain("components/compiler/clang/linux-x64/linux-x64");
    join_path(
        valid_root, sizeof(valid_root), components, "compiler/clang/linux-x64/linux-x64/22.1.5");
    make_valid_package(valid_root);

    make_parent_chain("components/unknown-component");
    make_parent_chain("components/compiler/not-a-tool");
    make_parent_chain("components/compiler/clang/not-a-host");
    make_parent_chain("components/compiler/clang/linux-x64/not-a-target");
    make_parent_chain("components/compiler/clang/linux-x64/linux-x64/bad version");
    make_parent_chain("components/compiler/clang/linux-x64/linux-x64/24.0.0");

    join_path(path, sizeof(path), components, "compiler/clang/linux-x64/linux-x64/23.0.0");
    write_text(path, "not a package directory\n");
    join_path(path, sizeof(path), components, "unexpected-file");
    write_text(path, "unexpected\n");

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_scan(&packages));
    TEST_ASSERT_TRUE(packages.complete);
    TEST_ASSERT_EQUAL_size_t(1, packages.count);
    TEST_ASSERT_EQUAL_size_t(1, packages.total_count);
    TEST_ASSERT_TRUE(packages.issue_count >= 7);
    TEST_ASSERT_EQUAL_size_t(packages.issue_count, packages.total_issue_count);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&identity, "compiler", "clang", "linux-x64", "linux-x64", "22.1.5"));
    TEST_ASSERT_TRUE(package_list_contains(&packages, &identity));
    TEST_ASSERT_FALSE(package_list_contains(NULL, &identity));
    TEST_ASSERT_FALSE(package_list_contains(&packages, NULL));

    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_COMPONENT, 0));
    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_TOOL, 0));
    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_HOST, 0));
    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_TARGET, 0));
    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_VERSION, 0));
    TEST_ASSERT_NOT_NULL(find_issue(&packages, PACKAGE_ISSUE_INVALID_PATH_TYPE, 1));

    invalid_content = find_issue(&packages, PACKAGE_ISSUE_INVALID_CONTENT, 1);
    TEST_ASSERT_NOT_NULL(invalid_content);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_quarantine(invalid_content, recovery_path, sizeof(recovery_path)));
    TEST_ASSERT_EQUAL_INT(0, access(recovery_path, F_OK));
    TEST_ASSERT_TRUE(access(invalid_content->path, F_OK) != 0);

    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(recovery_path, &exists));
    TEST_ASSERT_TRUE(exists);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_quarantine(NULL, recovery_path, sizeof(recovery_path)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_quarantine(invalid_content, NULL, 0));

    TEST_ASSERT_EQUAL_STRING("unexpected path type",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_PATH_TYPE));
    TEST_ASSERT_EQUAL_STRING("unknown component",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_COMPONENT));
    TEST_ASSERT_EQUAL_STRING("unknown tool", package_issue_reason_name(PACKAGE_ISSUE_INVALID_TOOL));
    TEST_ASSERT_EQUAL_STRING("invalid host platform",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_HOST));
    TEST_ASSERT_EQUAL_STRING("invalid target platform",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_TARGET));
    TEST_ASSERT_EQUAL_STRING("invalid package version",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_VERSION));
    TEST_ASSERT_EQUAL_STRING("invalid package contents",
                             package_issue_reason_name(PACKAGE_ISSUE_INVALID_CONTENT));
    TEST_ASSERT_EQUAL_STRING("unknown package issue",
                             package_issue_reason_name((PackageIssueReason)999));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_scan(NULL));
}

static void test_registry_platform(void) {
    char buffer[MAX_PLATFORM_LEN];
    char component[MAX_IDENTIFIER_LEN];
    char long_platform[MAX_PLATFORM_LEN + 16];

    memset(long_platform, 'a', sizeof(long_platform) - 1);
    long_platform[sizeof(long_platform) - 1] = '\0';

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, platform_get_host(NULL, sizeof(buffer)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, platform_get_host(buffer, 0));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, platform_get_host(buffer, 2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, platform_validate(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, platform_validate(""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, platform_validate("linux"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_ARCH, platform_validate("linux-riscv64"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, platform_validate(long_platform));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_component(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_component(""));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_tool(NULL, "clang"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_tool("compiler", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT,
                          registry_validate_tool("unknown", "clang"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, registry_validate_tool("linker", "ld"));
    TEST_ASSERT_TRUE(registry_is_component("compiler"));
    TEST_ASSERT_FALSE(registry_is_component(NULL));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          registry_find_tool_component(NULL, component, sizeof(component)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          registry_find_tool_component("clang", NULL, sizeof(component)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          registry_find_tool_component("clang", component, 0));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          registry_find_tool_component("clang", component, sizeof(component)));
    TEST_ASSERT_EQUAL_STRING("compiler", component);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          registry_find_tool_component("unknown", component, sizeof(component)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          registry_find_tool_component("clang-format", component, 2));
}

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_scope_validation);
    RUN_TEST(test_identity_validation);
    RUN_TEST(test_valid_package);
    RUN_TEST(test_invalid_package);
    RUN_TEST(test_metadata_paths);
    RUN_TEST(test_scan_roots);
    RUN_TEST(test_path_failures);
    RUN_TEST(test_scan_issues);
    RUN_TEST(test_registry_platform);
    return UNITY_END();
}
