/*
 * Test focus: Exercises wrapper-plan construction and exact bin reconciliation without repeating
 * the package lifecycle CLI workflow.
 */

#include "filesystem.h"
#include "package_metadata.h"
#include "layout.h"
#include "package.h"
#include "path.h"
#include "platform.h"
#include "system.h"
#include "wrappers.h"
#include "unity.h"
#include "test_platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static char root[MAX_PATH_LEN];
static CupError layout_result;
static CupError validate_result;
static CupError temp_result;
static CupError replace_result;
static SystemCommitState replace_state;
static CupError list_result;
static CupError executable_result;
static CupError is_executable_result;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree_real(const char *path) {
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        unlink(path);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[MAX_PATH_LEN];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        join_test_path(child, sizeof(child), path, entry->d_name);
        remove_tree_real(child);
    }
    closedir(directory);
    rmdir(path);
}

static void make_dir(const char *path) {
    TEST_ASSERT_TRUE(test_mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(content), fwrite(content, 1, strlen(content), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void reset_scenario(void) {
    char template_path[] = "/tmp/cup-wrappers-unit-XXXXXX";
    char bin[MAX_PATH_LEN];
    char packages[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(mkdtemp(template_path));
    strcpy(root, template_path);
    join_test_path(bin, sizeof(bin), root, "bin");
    join_test_path(packages, sizeof(packages), root, "packages");
    make_dir(bin);
    make_dir(packages);

    layout_result = CUP_OK;
    validate_result = CUP_OK;
    temp_result = CUP_OK;
    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    list_result = CUP_OK;
    executable_result = CUP_OK;
    is_executable_result = CUP_OK;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
    remove_tree_real(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError platform_get_host(char *buffer, size_t size) {
    return buffer_write_result(snprintf(buffer, size, "linux-x64"), size);
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *entry) {
    const char *separator = strchr(entry, '@');

    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    memcpy(identity->tool, entry, (size_t)(separator - entry));
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, separator + 1);
    return CUP_OK;
}

CupError package_identity_validate(const PackageIdentity *identity) {
    if (identity == NULL || identity->component[0] == '\0' || identity->tool[0] == '\0' ||
        identity->host_platform[0] == '\0' || identity->target_platform[0] == '\0' ||
        identity->version[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *package) {
    if (layout_result != CUP_OK) {
        return layout_result;
    }
    return buffer_write_result(
        snprintf(buffer,
                 size,
                 "%s/packages/%s-%s",
                 root,
                 package->tool,
                 package->target_platform),
        size);
}

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    (void)base_path;
    (void)identity;
    return validate_result;
}

CupError layout_get_bin_dir(char *buffer, size_t size) {
    if (layout_result != CUP_OK) {
        return layout_result;
    }
    return path_join(buffer, size, root, "bin");
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    if (layout_result != CUP_OK) {
        return layout_result;
    }
    return path_join(buffer, size, root, "bin/cup");
}

CupError filesystem_ensure_directory(const char *path) {
    make_dir(path);
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    int descriptor;
    int written;

    if (temp_result != CUP_OK) {
        return temp_result;
    }
    written = snprintf(path, path_size, "%s/%s-XXXXXX", directory, prefix);
    if (written < 0 || (size_t)written >= path_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return CUP_ERR_TEMPORARY;
    }
    *file = fdopen(descriptor, "w+b");
    if (*file == NULL) {
        close(descriptor);
        unlink(path);
        return CUP_ERR_TEMPORARY;
    }
    return CUP_OK;
}

CupError system_sync_file(FILE *file) {
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_set_executable(const char *path, int executable) {
    struct stat status;

    if (executable_result != CUP_OK) {
        return executable_result;
    }
    if (stat(path, &status) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    mode_t mode = executable ? status.st_mode | S_IXUSR : status.st_mode & (mode_t)~S_IXUSR;

    return chmod(path, mode) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = replace_state;
    if (replace_result != CUP_OK) {
        return replace_result;
    }
    return rename(source, destination) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_remove_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError filesystem_remove_tree(const char *path) {
    remove_tree_real(path);
    return CUP_OK;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    struct stat status;

    if (lstat(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (S_ISREG(status.st_mode)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (S_ISDIR(status.st_mode)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else if (S_ISLNK(status.st_mode)) {
        *kind = SYSTEM_PATH_LINK;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError system_is_executable(const char *path, int *is_executable) {
    struct stat status;

    if (is_executable_result != CUP_OK) {
        return is_executable_result;
    }
    if (stat(path, &status) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *is_executable = (status.st_mode & S_IXUSR) != 0;
    return CUP_OK;
}

CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    DIR *directory;
    struct dirent *entry;

    if (list_result != CUP_OK) {
        return list_result;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[MAX_PATH_LEN];
        SystemPathKind kind;
        CupError err;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        join_test_path(child, sizeof(child), path, entry->d_name);
        err = system_get_path_kind(child, &kind);
        if (err == CUP_OK) {
            err = callback(child, kind, userdata);
        }
        if (err != CUP_OK) {
            closedir(directory);
            return err;
        }
    }
    closedir(directory);
    return CUP_OK;
}

static void write_package_info(const char *tool, const char *target, const char *content) {
    char package_dir[MAX_PATH_LEN];
    char package_metadata_path[MAX_PATH_LEN];

    TEST_ASSERT_TRUE(
        snprintf(package_dir, sizeof(package_dir), "%s/packages/%s-%s", root, tool, target) > 0);
    make_dir(package_dir);
    join_test_path(
        package_metadata_path, sizeof(package_metadata_path), package_dir, CUP_INFO_FILENAME);
    write_file(package_metadata_path, content);
}

static PackageIdentity default_entry(const char *tool, const char *target) {
    PackageIdentity entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.component, "compiler");
    strcpy(entry.tool, tool);
    strcpy(entry.host_platform, "linux-x64");
    strcpy(entry.target_platform, target);
    strcpy(entry.version, "22.1.5");
    return entry;
}

static WrapperPlan simple_plan(void) {
    WrapperPlan plan;

    wrapper_plan_init(&plan);
    plan.items = calloc(1, sizeof(*plan.items));
    TEST_ASSERT_NOT_NULL(plan.items);
    plan.count = 1;
    plan.capacity = 1;
    strcpy(plan.items[0].name, "clang");
    strcpy(plan.items[0].target, "../components/compiler/clang/bin/cla'ng");
    return plan;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_plan_lifetime(void) {
    WrapperPlan plan;

    memset(&plan, 0xff, sizeof(plan));
    wrapper_plan_init(&plan);
    TEST_ASSERT_NULL(plan.items);
    TEST_ASSERT_EQUAL_INT(0, (int)plan.count);
    wrapper_plan_free(NULL);
    plan.items = malloc(sizeof(*plan.items));
    plan.count = 1;
    plan.capacity = 1;
    wrapper_plan_free(&plan);
    TEST_ASSERT_NULL(plan.items);
    TEST_ASSERT_EQUAL_INT(0, (int)plan.count);
}

static void test_build_active(void) {
    WrapperPlan plan;
    PackageIdentity entry = default_entry("clang", "linux-x64");

    write_package_info("clang", "linux-x64", "entry.clang=bin/clang\nentry.clang++=bin/clang++\n");
    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build_active(&plan, &entry));
    TEST_ASSERT_EQUAL_INT(2, (int)plan.count);
    TEST_ASSERT_EQUAL_STRING("clang", plan.items[0].name);
    TEST_ASSERT_TRUE(strstr(plan.items[0].target, "bin/clang") != NULL);
    wrapper_plan_free(&plan);
}

static void test_build_scopes(void) {
    WrapperPlan plan;
    CupState state = {0};

    state.active[state.active_count++] = default_entry("clang", "linux-x64");
    state.active[state.active_count++] = default_entry("gcc", "windows-x64");
    state.active[state.active_count] = default_entry("lld", "linux-x64");
    strcpy(state.active[state.active_count++].host_platform, "macos-x64");
    write_package_info("clang", "linux-x64", "entry.clang=bin/clang\n");
    write_package_info("gcc", "windows-x64", "entry.gcc=bin/gcc\n");

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build(&plan, &state));
    TEST_ASSERT_EQUAL_INT(2, (int)plan.count);
    TEST_ASSERT_EQUAL_STRING("clang", plan.items[0].name);
    TEST_ASSERT_EQUAL_STRING("windows-x64-gcc", plan.items[1].name);
    wrapper_plan_free(&plan);
}

static void test_build_conflicts(void) {
    WrapperPlan plan;
    CupState state = {0};

    state.active[state.active_count++] = default_entry("clang", "linux-x64");
    state.active[state.active_count++] = default_entry("clang", "linux-x64");
    write_package_info("clang", "linux-x64", "entry.cc=bin/clang\n");

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build(&plan, &state));
    TEST_ASSERT_EQUAL_INT(1, (int)plan.count);
    wrapper_plan_free(&plan);

    state.active_count = 0;
    state.active[state.active_count++] = default_entry("clang", "linux-x64");
    state.active[state.active_count++] = default_entry("gcc", "linux-x64");
    write_package_info("gcc", "linux-x64", "entry.cc=bin/gcc\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, wrapper_plan_build(&plan, &state));
    wrapper_plan_free(&plan);

    state.active_count = 1;
    write_package_info("clang", "linux-x64", "entry.cup=bin/clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, wrapper_plan_build(&plan, &state));
    wrapper_plan_free(&plan);
}

static void test_build_failures(void) {
    WrapperPlan plan;
    CupState state = {0};

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(&plan, NULL));

    state.active[state.active_count++] = default_entry("clang", "linux-x64");
    state.active[0].version[0] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(&plan, &state));

    state.active[0] = default_entry("clang", "linux-x64");
    layout_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_build(&plan, &state));
    layout_result = CUP_OK;
    validate_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, wrapper_plan_build(&plan, &state));
    wrapper_plan_free(&plan);
}

static void test_apply_and_check(void) {
    WrapperPlan plan = simple_plan();
    char wrapper[MAX_PATH_LEN];
    char stale[MAX_PATH_LEN];
    char stale_child[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    int matches;
    size_t issues;

    join_test_path(binary, sizeof(binary), root, "bin/cup");
    write_file(binary, "cup");
    join_test_path(stale, sizeof(stale), root, "bin/stale");
    make_dir(stale);
    join_test_path(stale_child, sizeof(stale_child), stale, "child");
    write_file(stale_child, "stale");

    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));
    join_test_path(wrapper, sizeof(wrapper), root, "bin/clang");
    TEST_ASSERT_TRUE(access(wrapper, X_OK) == 0);
    TEST_ASSERT_TRUE(access(stale, F_OK) != 0);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_check(&plan, &issues));
    TEST_ASSERT_EQUAL_INT(0, (int)issues);

    TEST_ASSERT_EQUAL_INT(0, chmod(wrapper, 0600));
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(0, chmod(wrapper, 0700));
    TEST_ASSERT_EQUAL_INT(0, unlink(wrapper));
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));

    write_file(wrapper, "broken");
    join_test_path(stale, sizeof(stale), root, "bin/other");
    write_file(stale, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_check(&plan, &issues));
    TEST_ASSERT_EQUAL_INT(2, (int)issues);
    wrapper_plan_free(&plan);
}

static void test_apply_failures(void) {
    WrapperPlan plan = simple_plan();
    int matches;
    size_t issues;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build_active(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_apply(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_expected_matches(NULL, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_check(&plan, NULL));

    temp_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, wrapper_plan_apply(&plan));
    temp_result = CUP_OK;
    replace_result = CUP_ERR_FILESYSTEM;
    replace_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_apply(&plan));
    replace_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, wrapper_plan_apply(&plan));

    layout_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    wrapper_plan_free(&plan);
}

static void test_scan_failures(void) {
    WrapperPlan plan = simple_plan();
    char binary[MAX_PATH_LEN];
    int matches;
    size_t issues;

    join_test_path(binary, sizeof(binary), root, "bin/cup");
    write_file(binary, "cup");
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));

    is_executable_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_expected_matches(&plan, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    is_executable_result = CUP_OK;

    list_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_apply(&plan));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    wrapper_plan_free(&plan);
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plan_lifetime);
    RUN_TEST(test_build_active);
    RUN_TEST(test_build_scopes);
    RUN_TEST(test_build_conflicts);
    RUN_TEST(test_build_failures);
    RUN_TEST(test_apply_and_check);
    RUN_TEST(test_apply_failures);
    RUN_TEST(test_scan_failures);
    return UNITY_END();
}
