/*
 * Exercises wrapper-plan construction and exact bin reconciliation without repeating
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

#if !defined(_WIN32)
#include <dirent.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int executable_override;
static int operation_sequence;
static int mode_sequence;
static int sync_sequence;
static int namespace_case_sensitive;

#if defined(_WIN32)
#define TEST_HOST "windows-x64"
#define TEST_OTHER_HOST "linux-x64"
#define TEST_OTHER_TARGET "linux-x64"
#define TEST_WRAPPER_SUFFIX ".cmd"
#define TEST_BINARY_NAME "cup.exe"
#else
#define TEST_HOST "linux-x64"
#define TEST_OTHER_HOST "macos-x64"
#define TEST_OTHER_TARGET "windows-x64"
#define TEST_WRAPPER_SUFFIX ""
#define TEST_BINARY_NAME "cup"
#endif

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void join_test_path(char *buffer, size_t size, const char *left, const char *right) {
    int written = snprintf(buffer, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void remove_tree_real(const char *path) {
    TEST_ASSERT_EQUAL_INT(0, test_remove_tree(path));
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

static CupError wrapper_plan_exact_matches(const WrapperPlan *plan, int *matches) {
    CupError err;
    size_t issues;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = wrapper_plan_check(plan, &issues);
    if (err != CUP_OK) {
        return err;
    }

    *matches = issues == 0;
    return CUP_OK;
}

static void reset_scenario(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];
    char bin[MAX_PATH_LEN];
    char packages[MAX_PATH_LEN];

    TEST_ASSERT_NOT_NULL(
        test_make_temp_directory(template_path, sizeof(template_path), "cup-wrappers-unit"));
    TEST_ASSERT_TRUE(strlen(template_path) < sizeof(root));
    strcpy(root, template_path);
    join_test_path(bin, sizeof(bin), root, "bin");
    join_test_path(packages, sizeof(packages), root, "packages");
    make_dir(bin);
    make_dir(packages);

    {
        char case_probe[MAX_PATH_LEN];
        char case_alternate[MAX_PATH_LEN];

        join_test_path(case_probe, sizeof(case_probe), root, ".cup-case-probe");
        join_test_path(case_alternate, sizeof(case_alternate), root, ".CUP-CASE-PROBE");
        write_file(case_probe, "probe");
        namespace_case_sensitive = !test_access_exists(case_alternate);
        TEST_ASSERT_EQUAL_INT(0, test_unlink(case_probe));
    }

    layout_result = CUP_OK;
    validate_result = CUP_OK;
    temp_result = CUP_OK;
    replace_result = CUP_OK;
    replace_state = SYSTEM_COMMIT_DURABLE;
    list_result = CUP_OK;
    executable_result = CUP_OK;
    is_executable_result = CUP_OK;
    executable_override = -1;
    operation_sequence = 0;
    mode_sequence = 0;
    sync_sequence = 0;
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
    return buffer_write_result(snprintf(buffer, size, "%s", TEST_HOST), size);
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *entry,
                                        FILE *diagnostics) {
    (void)diagnostics;
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

CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;
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

void validated_package_init(ValidatedPackage *package) {
    TEST_ASSERT_NOT_NULL(package);
    memset(package, 0, sizeof(*package));
    package_metadata_init(&package->metadata);
}

void validated_package_free(ValidatedPackage *package) {
    if (package != NULL) {
        package_metadata_free(&package->metadata);
        memset(package, 0, sizeof(*package));
    }
}

CupError validated_package_load(ValidatedPackage *package,
                                const char *base_path,
                                const PackageIdentity *identity,
                                FILE *diagnostics) {
    (void)diagnostics;
    char metadata_path[MAX_PATH_LEN];
    CupError err;

    TEST_ASSERT_NOT_NULL(package);
    TEST_ASSERT_NOT_NULL(base_path);
    TEST_ASSERT_NOT_NULL(identity);
    if (validate_result != CUP_OK) {
        return validate_result;
    }
    err = path_join(metadata_path, sizeof(metadata_path), base_path, CUP_INFO_FILENAME);
    if (err == CUP_OK) {
        err = package_metadata_load(&package->metadata, metadata_path, stderr);
    }
    return err;
}

CupError layout_get_root(char *buffer, size_t size) {
    if (layout_result != CUP_OK) {
        return layout_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", root), size);
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
    return path_join(buffer, size, root, "bin/" TEST_BINARY_NAME);
}

CupError filesystem_ensure_directory(const char *path) {
    make_dir(path);
    return CUP_OK;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    if (temp_result != CUP_OK) {
        return temp_result;
    }
    return test_create_temp_file(directory, prefix, path, path_size, file) == 0
               ? CUP_OK
               : CUP_ERR_TEMPORARY;
}

CupError system_set_file_executable(FILE *file, int executable) {
    if (executable_result != CUP_OK) {
        return executable_result;
    }
    if (file == NULL || !executable) {
        return CUP_ERR_INVALID_INPUT;
    }
    mode_sequence = ++operation_sequence;
#if defined(_WIN32)
    return test_file_descriptor(file) >= 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
#else
    {
        struct stat status;
        int descriptor = test_file_descriptor(file);

        if (descriptor < 0 || fstat(descriptor, &status) != 0) {
            return CUP_ERR_FILESYSTEM;
        }
        return fchmod(descriptor, status.st_mode | S_IXUSR) == 0 ? CUP_OK
                                                                 : CUP_ERR_FILESYSTEM;
    }
#endif
}

CupError system_sync_file(FILE *file) {
    sync_sequence = ++operation_sequence;
    return fflush(file) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_set_executable(const char *path, int executable) {
    if (executable_result != CUP_OK) {
        return executable_result;
    }
#if defined(_WIN32)
    if (executable && strstr(path, ".cmd") == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    return test_access_exists(path) ? CUP_OK : CUP_ERR_FILESYSTEM;
#else
    {
        TestPlatformStat status;
        mode_t mode;

        if (test_stat_path(path, &status) != 0) {
            return CUP_ERR_FILESYSTEM;
        }
        mode = executable ? status.st_mode | S_IXUSR : status.st_mode & (mode_t)~S_IXUSR;
        return chmod(path, mode) == 0 ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
#endif
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (replace_result != CUP_OK) {
        *state = replace_state;
        return replace_result;
    }
    if (test_replace_file(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = replace_state;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError filesystem_replace_file_atomically(const char *directory,
                                            const char *temporary_prefix,
                                            const char *destination,
                                            int executable,
                                            FilesystemFileWriter writer,
                                            const void *value) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;

    err = system_create_temp_file(
        directory, temporary_prefix, temporary, sizeof(temporary), &file);
    if (err == CUP_OK) {
        err = writer(file, value);
    }
    if (err == CUP_OK && executable) {
        err = system_set_file_executable(file, 1);
    }
    if (err == CUP_OK) {
        err = system_sync_file(file);
    }
    if (file != NULL) {
        if (fclose(file) != 0 && err == CUP_OK) {
            err = CUP_ERR_FILESYSTEM;
        }
        file = NULL;
    }
    if (err != CUP_OK) {
        (void)system_remove_file(temporary);
        return err;
    }

    err = system_replace_file(temporary, destination, &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        (void)system_remove_file(temporary);
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;
}

CupError system_remove_path_if_identity(const char *path,
                                        const SystemPathIdentity *expected_identity,
                                        int (*cancelled)(void)) {
    if (expected_identity == NULL || !expected_identity->valid) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (cancelled != NULL && cancelled()) {
        return CUP_ERR_INTERRUPT;
    }
    if (expected_identity->kind == SYSTEM_PATH_DIRECTORY) {
        remove_tree_real(path);
        return CUP_OK;
    }
    return system_remove_file(path);
}

CupError filesystem_remove_tree(const char *path) {
    remove_tree_real(path);
    return CUP_OK;
}

CupError system_get_path_kind(const char *path, SystemPathKind *kind) {
    TestPlatformStat status;

    if (test_stat_path(path, &status) != 0) {
        *kind = errno == ENOENT ? SYSTEM_PATH_MISSING : SYSTEM_PATH_OTHER;
        return errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
    }
    if (test_stat_is_regular(&status)) {
        *kind = SYSTEM_PATH_REGULAR_FILE;
    } else if (test_stat_is_directory(&status)) {
        *kind = SYSTEM_PATH_DIRECTORY;
    } else {
        *kind = SYSTEM_PATH_OTHER;
    }
    return CUP_OK;
}

CupError system_get_path_identity(const char *path, SystemPathIdentity *identity) {
    const char *name;
    SystemPathKind kind;
    CupError err;

    if (path == NULL || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    name = path_last_segment(path);
    if (name != NULL && strcmp(name, CUP_ROOT_MARKER_FILENAME) == 0) {
        identity->volume = 2u;
        identity->object = 2u;
        identity->kind = SYSTEM_PATH_REGULAR_FILE;
        identity->valid = 1;
        return CUP_OK;
    }
    if (name != NULL && strcmp(name, "ROOT.TXT") == 0) {
        if (!namespace_case_sensitive) {
            identity->volume = 2u;
            identity->object = 2u;
            identity->kind = SYSTEM_PATH_REGULAR_FILE;
            identity->valid = 1;
        }
        return CUP_OK;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    identity->volume = 1u;
    identity->object = 1u;
    identity->kind = kind;
    identity->valid = 1;
    return CUP_OK;
}

CupError system_is_executable(const char *path, int *is_executable) {
    TestPlatformStat status;

    if (is_executable_result != CUP_OK) {
        return is_executable_result;
    }
    if (test_stat_path(path, &status) != 0 || !test_stat_is_regular(&status)) {
        return CUP_ERR_FILESYSTEM;
    }
    if (executable_override >= 0) {
        *is_executable = executable_override;
        return CUP_OK;
    }
#if defined(_WIN32)
    *is_executable = strlen(path) >= 4 && _stricmp(path + strlen(path) - 4, ".cmd") == 0;
#else
    *is_executable = (status.st_mode & S_IXUSR) != 0;
#endif
    return CUP_OK;
}

CupError system_list_directory(const char *path, SystemDirectoryCallback callback, void *userdata) {
    uint64_t identity_index = 1;

    if (list_result != CUP_OK) {
        return list_result;
    }
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA data;
        char pattern[MAX_PATH_LEN];
        HANDLE handle;

        join_test_path(pattern, sizeof(pattern), path, "*");
        handle = FindFirstFileA(pattern, &data);
        if (handle == INVALID_HANDLE_VALUE) {
            return CUP_ERR_FILESYSTEM;
        }
        do {
            char child[MAX_PATH_LEN];
            SystemPathKind kind;
            CupError err;

            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
                continue;
            }
            join_test_path(child, sizeof(child), path, data.cFileName);
            err = system_get_path_kind(child, &kind);
            if (err == CUP_OK) {
                {
                    SystemPathIdentity identity = {1u, identity_index++, 0u, kind, 1};
                    err = callback(child, kind, &identity, userdata);
                }
            }
            if (err != CUP_OK) {
                FindClose(handle);
                return err;
            }
        } while (FindNextFileA(handle, &data));
        FindClose(handle);
        return CUP_OK;
    }
#else
    {
        DIR *directory = opendir(path);
        struct dirent *entry;

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
                {
                    SystemPathIdentity identity = {1u, identity_index++, 0u, kind, 1};
                    err = callback(child, kind, &identity, userdata);
                }
            }
            if (err != CUP_OK) {
                closedir(directory);
                return err;
            }
        }
        closedir(directory);
        return CUP_OK;
    }
#endif
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
    strcpy(entry.host_platform, TEST_HOST);
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
    snprintf(plan.items[0].name, sizeof(plan.items[0].name), "clang%s", TEST_WRAPPER_SUFFIX);
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

static void test_build_default(void) {
    WrapperPlan plan;
    PackageIdentity entry = default_entry("clang", TEST_HOST);

    write_package_info("clang", TEST_HOST, "entry.clang=bin/clang\nentry.clang++=bin/clang++\n");
    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build_default(&plan, &entry));
    TEST_ASSERT_EQUAL_INT(2, (int)plan.count);
    {
        char expected[MAX_COMMAND_NAME_LEN];
        snprintf(expected, sizeof(expected), "clang%s", TEST_WRAPPER_SUFFIX);
        TEST_ASSERT_EQUAL_STRING(expected, plan.items[0].name);
    }
    TEST_ASSERT_TRUE(strstr(plan.items[0].target, "bin/clang") != NULL);
    wrapper_plan_free(&plan);
}

static void test_build_scopes(void) {
    WrapperPlan plan;
    CupState state = {0};

    state.defaults[state.default_count++] = default_entry("clang", TEST_HOST);
    state.defaults[state.default_count++] = default_entry("gcc", TEST_OTHER_TARGET);
    state.defaults[state.default_count] = default_entry("lld", TEST_HOST);
    strcpy(state.defaults[state.default_count++].host_platform, TEST_OTHER_HOST);
    write_package_info("clang", TEST_HOST, "entry.clang=bin/clang\n");
    write_package_info("gcc", TEST_OTHER_TARGET, "entry.gcc=bin/gcc\n");

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build(&plan, &state));
    TEST_ASSERT_EQUAL_INT(2, (int)plan.count);
    {
        char expected[MAX_COMMAND_NAME_LEN];
        snprintf(expected, sizeof(expected), "clang%s", TEST_WRAPPER_SUFFIX);
        TEST_ASSERT_EQUAL_STRING(expected, plan.items[0].name);
    }
    {
        char expected[MAX_COMMAND_NAME_LEN];
        snprintf(expected, sizeof(expected), "%s-gcc%s", TEST_OTHER_TARGET, TEST_WRAPPER_SUFFIX);
        TEST_ASSERT_EQUAL_STRING(expected, plan.items[1].name);
    }
    wrapper_plan_free(&plan);
}

static void test_build_conflicts(void) {
    WrapperPlan plan;
    CupState state = {0};

    state.defaults[state.default_count++] = default_entry("clang", TEST_HOST);
    state.defaults[state.default_count++] = default_entry("clang", TEST_HOST);
    write_package_info("clang", TEST_HOST, "entry.cc=bin/clang\n");

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_build(&plan, &state));
    TEST_ASSERT_EQUAL_INT(1, (int)plan.count);
    wrapper_plan_free(&plan);

    state.default_count = 0;
    state.defaults[state.default_count++] = default_entry("clang", TEST_HOST);
    state.defaults[state.default_count++] = default_entry("gcc", TEST_HOST);
    write_package_info("gcc", TEST_HOST, "entry.cc=bin/gcc\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, wrapper_plan_build(&plan, &state));
    wrapper_plan_free(&plan);

    state.default_count = 1;
    write_package_info("clang", TEST_HOST, "entry.cup=bin/clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, wrapper_plan_build(&plan, &state));
    wrapper_plan_free(&plan);
}

static void test_build_failures(void) {
    WrapperPlan plan;
    CupState state = {0};

    wrapper_plan_init(&plan);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(&plan, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build_default(&plan, NULL));

    state.defaults[state.default_count++] = default_entry("clang", TEST_HOST);
    state.defaults[0].version[0] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(&plan, &state));

    state.defaults[0] = default_entry("clang", TEST_HOST);
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

    join_test_path(binary, sizeof(binary), root, "bin/" TEST_BINARY_NAME);
    write_file(binary, "cup");
    join_test_path(stale, sizeof(stale), root, "bin/stale");
    make_dir(stale);
    join_test_path(stale_child, sizeof(stale_child), stale, "child");
    write_file(stale_child, "stale");
    {
        char expected_directory[MAX_PATH_LEN];
        char name[sizeof(PublicCommandName)];
        snprintf(name, sizeof(name), "clang%s", TEST_WRAPPER_SUFFIX);
        join_test_path(expected_directory, sizeof(expected_directory), root, "bin");
        join_test_path(wrapper, sizeof(wrapper), expected_directory, name);
        make_dir(wrapper);
    }

    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));
    TEST_ASSERT_TRUE(mode_sequence > 0);
    TEST_ASSERT_TRUE(sync_sequence > mode_sequence);
    {
        char name[MAX_COMMAND_NAME_LEN];
        snprintf(name, sizeof(name), "bin/clang%s", TEST_WRAPPER_SUFFIX);
        join_test_path(wrapper, sizeof(wrapper), root, name);
    }
    {
        int is_executable;
        TEST_ASSERT_EQUAL_INT(CUP_OK, system_is_executable(wrapper, &is_executable));
        TEST_ASSERT_TRUE(is_executable);
    }
    TEST_ASSERT_FALSE(test_access_exists(stale));
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_check(&plan, &issues));
    TEST_ASSERT_EQUAL_INT(0, (int)issues);

#if !defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, chmod(wrapper, 0600));
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(0, chmod(wrapper, 0700));
#endif
    TEST_ASSERT_EQUAL_INT(0, test_unlink(wrapper));
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));

    join_test_path(stale, sizeof(stale), root, "bin/other");
    write_file(stale, "stale");
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_entries_match(&plan, &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);

    write_file(wrapper, "broken");
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_entries_match(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_FALSE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_check(&plan, &issues));
    TEST_ASSERT_EQUAL_INT(2, (int)issues);
    wrapper_plan_free(&plan);
}

static void test_empty_plan_reconciles_bin(void) {
    WrapperPlan plan;
    char binary[MAX_PATH_LEN];
    char stale[MAX_PATH_LEN];
    int matches = 0;
    size_t issues = 99;

    wrapper_plan_init(&plan);
    join_test_path(binary, sizeof(binary), root, "bin/" TEST_BINARY_NAME);
    write_file(binary, "cup");
    join_test_path(stale, sizeof(stale), root, "bin/stale");
    write_file(stale, "stale");
#if !defined(_WIN32)
    if (namespace_case_sensitive) {
        char case_alias[MAX_PATH_LEN];
        join_test_path(case_alias, sizeof(case_alias), root, "bin/CUP");
        write_file(case_alias, "stale alias");
    }
#endif

    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));
    TEST_ASSERT_TRUE(test_access_exists(binary));
    TEST_ASSERT_FALSE(test_access_exists(stale));
#if !defined(_WIN32)
    {
        char case_alias[MAX_PATH_LEN];
        join_test_path(case_alias, sizeof(case_alias), root, "bin/CUP");
        if (namespace_case_sensitive) {
            TEST_ASSERT_FALSE(test_access_exists(case_alias));
        } else {
            TEST_ASSERT_TRUE(test_access_exists(case_alias));
        }
    }
#endif
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_TRUE(matches);
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_check(&plan, &issues));
    TEST_ASSERT_EQUAL_size_t(0, issues);
    wrapper_plan_free(&plan);
}

static void test_apply_failures(void) {
    WrapperPlan plan = simple_plan();
    int matches;
    size_t issues;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_build_default(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_apply(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_entries_match(NULL, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_entries_match(&plan, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_exact_matches(NULL, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_exact_matches(&plan, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, wrapper_plan_check(NULL, &issues));
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
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    wrapper_plan_free(&plan);
}

static void test_scan_failures(void) {
    WrapperPlan plan = simple_plan();
    char binary[MAX_PATH_LEN];
#if !defined(_WIN32)
    int matches;
#endif
    size_t issues;

    join_test_path(binary, sizeof(binary), root, "bin/" TEST_BINARY_NAME);
    write_file(binary, "cup");
    TEST_ASSERT_EQUAL_INT(CUP_OK, wrapper_plan_apply(&plan));

#if !defined(_WIN32)
    is_executable_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_exact_matches(&plan, &matches));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    is_executable_result = CUP_OK;
#endif

    list_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_apply(&plan));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, wrapper_plan_check(&plan, &issues));
    wrapper_plan_free(&plan);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plan_lifetime);
    RUN_TEST(test_build_default);
    RUN_TEST(test_build_scopes);
    RUN_TEST(test_build_conflicts);
    RUN_TEST(test_build_failures);
    RUN_TEST(test_apply_and_check);
    RUN_TEST(test_empty_plan_reconciles_bin);
    RUN_TEST(test_apply_failures);
    RUN_TEST(test_scan_failures);
    return UNITY_END();
}
