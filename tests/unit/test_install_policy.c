/*
 * Tests the scoped official installation policy and user preference overlay.
 */
#include "install_policy.h"
#include "filesystem.h"
#include "tool_preferences.h"
#include "layout.h"
#include "registry.h"
#include "platform.h"
#include "system.h"
#include "text.h"
#include "unity.h"
#include "test_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char root[MAX_PATH_LEN];
static char preferences_path[MAX_PATH_LEN];
static int sync_parent_calls;
static CupError sync_parent_result;

static void path_join(char *out, size_t size, const char *left, const char *right) {
    int written = snprintf(out, size, "%s/%s", left, right);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *text;
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    size = ftell(file);
    TEST_ASSERT_TRUE(size >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    text = malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_size_t((size_t)size, fread(text, 1, (size_t)size, file));
    TEST_ASSERT_FALSE(ferror(file));
    text[size] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    return text;
}

void setUp(void) {
    char template_path[CUP_TEST_TEMP_PATH_SIZE];
    TEST_ASSERT_NOT_NULL(test_make_temp_directory(
        template_path, sizeof(template_path), "cup-policy-unit"));
    strcpy(root, template_path);
    path_join(preferences_path, sizeof(preferences_path), root, "preferences.txt");
    sync_parent_calls = 0;
    sync_parent_result = CUP_OK;
}

void tearDown(void) {
    (void)test_unlink(preferences_path);
    (void)test_rmdir(root);
}

CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host,
                            const char *target) {
    if (scope == NULL || text_is_empty(host) || text_is_empty(target) ||
        registry_validate_component(component) != CUP_OK || platform_validate(host) != CUP_OK ||
        platform_validate(target) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(scope, 0, sizeof(*scope));
    if (text_copy(scope->component, sizeof(scope->component), component) != CUP_OK ||
        text_copy(scope->host_platform, sizeof(scope->host_platform), host) != CUP_OK ||
        text_copy(scope->target_platform, sizeof(scope->target_platform), target) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

int package_scope_equals(const PackageScope *left, const PackageScope *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0;
}

CupError platform_validate(const char *platform) {
    static const char *const values[] = {
        "linux-x64", "linux-arm64", "windows-x64", "macos-x64", "macos-arm64"};
    size_t i;
    if (platform == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (strcmp(values[i], platform) == 0) {
            return CUP_OK;
        }
    }
    return CUP_ERR_INVALID_INPUT;
}

int platform_is_supported(const char *platform) {
    return platform_validate(platform) == CUP_OK;
}

CupError platform_get_host(char *buffer, size_t size) {
    return text_copy(buffer, size, "linux-x64");
}

CupError layout_get_preferences_path(char *buffer, size_t size) {
    return text_copy(buffer, size, preferences_path);
}

CupError layout_get_config_dir(char *buffer, size_t size) {
    return text_copy(buffer, size, root);
}

CupError layout_ensure_config(void) {
    return CUP_OK;
}

CupError system_path_exists(const char *path, int *exists) {
    TestPlatformStat info;
    if (path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (test_stat_path(path, &info) == 0) {
        *exists = 1;
        return CUP_OK;
    }
    if (errno == ENOENT) {
        *exists = 0;
        return CUP_OK;
    }
    return CUP_ERR_FILESYSTEM;
}

CupError system_remove_file(const char *path) {
    return test_unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    if (path == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    sync_parent_calls++;
    return sync_parent_result;
}

CupError filesystem_replace_file_atomically(const char *directory,
                                            const char *temporary_prefix,
                                            const char *destination,
                                            int executable,
                                            FilesystemFileWriter writer,
                                            const void *value) {
    FILE *file;
    CupError err;

    if (directory == NULL || temporary_prefix == NULL || destination == NULL ||
        executable != 0 || writer == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    file = fopen(destination, "wb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    err = writer(file, value);
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err != CUP_OK) {
        (void)test_unlink(destination);
    }
    return err;
}

static void test_compiled_policy(void) {
    InstallPolicy policy;
    const InstallDefault *official;
    const InstallNamedList *profile;
    const InstallNamedList *toolchain;

    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load(&policy));
    official = install_policy_find_default(&policy, "linux-x64", "linux-x64", "compiler");
    TEST_ASSERT_NOT_NULL(official);
    TEST_ASSERT_EQUAL_STRING("clang", official->tool);
    official = install_policy_find_default(&policy, "linux-x64", "windows-x64", "compiler");
    TEST_ASSERT_NOT_NULL(official);
    TEST_ASSERT_EQUAL_STRING("gcc", official->tool);
    official = install_policy_find_default(&policy, "linux-x64", "windows-x64", "linker");
    TEST_ASSERT_NOT_NULL(official);
    TEST_ASSERT_EQUAL_STRING("ld", official->tool);
    TEST_ASSERT_NULL(install_policy_find_default(&policy, "windows-x64", "linux-x64", "compiler"));

    profile = install_policy_find_profile(&policy, "standard");
    TEST_ASSERT_NOT_NULL(profile);
    TEST_ASSERT_EQUAL_size_t(4, profile->item_count);
    toolchain = install_policy_find_toolchain(&policy, "gnu");
    TEST_ASSERT_NOT_NULL(toolchain);
    TEST_ASSERT_EQUAL_size_t(3, toolchain->item_count);

    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load(&policy));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, install_policy_load(NULL));
}

static void test_policy_lookup_contracts(void) {
    InstallPolicy policy;

    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load(&policy));
    install_policy_init(NULL);
    TEST_ASSERT_NULL(install_policy_find_default(NULL, "linux-x64", "linux-x64", "compiler"));
    TEST_ASSERT_NULL(install_policy_find_default(&policy, NULL, "linux-x64", "compiler"));
    TEST_ASSERT_NULL(install_policy_find_default(&policy, "invalid", "linux-x64", "compiler"));
    TEST_ASSERT_NULL(install_policy_find_profile(NULL, "minimal"));
    TEST_ASSERT_NULL(install_policy_find_profile(&policy, "missing"));
    TEST_ASSERT_NULL(install_policy_find_toolchain(NULL, "llvm"));
    TEST_ASSERT_NULL(install_policy_find_toolchain(&policy, "missing"));
}

static void test_preference_mutation_and_lookup(void) {
    ToolPreferences preferences;
    const ToolPreference *preference;
    int removed = 0;
    size_t removed_count = 0;

    tool_preferences_init(&preferences);
    TEST_ASSERT_NULL(tool_preferences_find(&preferences, "linux-x64", "compiler"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "linker", "ld"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "windows-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_size_t(3, preferences.count);
    preference = tool_preferences_find(&preferences, "linux-x64", "compiler");
    TEST_ASSERT_NOT_NULL(preference);
    TEST_ASSERT_EQUAL_STRING("gcc", preference->tool);

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_reset(&preferences, "linux-x64", "linux-x64", "compiler", &removed));
    TEST_ASSERT_TRUE(removed);
    TEST_ASSERT_NULL(tool_preferences_find(&preferences, "linux-x64", "compiler"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_reset_scope(&preferences, "linux-x64", "linux-x64", &removed_count));
    TEST_ASSERT_EQUAL_size_t(1, removed_count);
    TEST_ASSERT_EQUAL_size_t(1, preferences.count);
    TEST_ASSERT_EQUAL_STRING("windows-x64", preferences.items[0].scope.target_platform);

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        tool_preferences_set(&preferences, "windows-x64", "linux-x64", "compiler", "clang"));
}

static void test_preferences_round_trip_format2(void) {
    ToolPreferences preferences;
    ToolPreferences loaded;
    char *text;

    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "windows-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "linker", "ld"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_save(&preferences));
    text = read_text(preferences_path);
    TEST_ASSERT_EQUAL_STRING("format=2\n"
                             "preferred.linux-x64.compiler=gcc\n"
                             "preferred.linux-x64.linker=ld\n"
                             "preferred.windows-x64.compiler=gcc\n",
                             text);
    free(text);

    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_load(&loaded, stderr));
    TEST_ASSERT_EQUAL_size_t(3, loaded.count);
    TEST_ASSERT_EQUAL_STRING("linux-x64", loaded.items[0].scope.host_platform);
}

static void test_preferences_invalid_and_missing(void) {
    ToolPreferences preferences;

    (void)test_unlink(preferences_path);
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_load(&preferences, stderr));
    TEST_ASSERT_EQUAL_size_t(0, preferences.count);

    write_text(preferences_path, "format=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
    write_text(preferences_path, "format=2\npreferred.linux-x64.compiler=clang\npreferred.linux-x64.compiler=gcc\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
    write_text(preferences_path, "format=2\npreferred.linux-x64.compiler=gdb\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
    write_text(preferences_path, "format=2\npreferred.linux-x64.linux-x64.compiler=clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
    write_text(preferences_path, "format=2\npreferred.invalid.compiler=clang\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
    write_text(preferences_path, "");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&preferences, stderr));
}

static void test_invalid_preferences_save_and_capacity(void) {
    ToolPreferences preferences;
    size_t i;

    tool_preferences_init(&preferences);
    preferences.count = MAX_TOOL_PREFERENCES + 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_save(&preferences));
    TEST_ASSERT_NULL(tool_preferences_find(&preferences, "linux-x64", "compiler"));

    tool_preferences_init(&preferences);
    for (i = 0; i < MAX_TOOL_PREFERENCES; ++i) {
        preferences.items[i].scope.component[0] = 'x';
        preferences.items[i].scope.component[1] = '\0';
    }
    preferences.count = MAX_TOOL_PREFERENCES;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "clang"));
}

static void test_empty_save(void) {
    ToolPreferences preferences;
    int exists = 0;

    write_text(preferences_path, "format=2\n");
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_save(&preferences));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(preferences_path, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(1, sync_parent_calls);

    write_text(preferences_path, "format=2\n");
    sync_parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, tool_preferences_save(&preferences));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_compiled_policy);
    RUN_TEST(test_policy_lookup_contracts);
    RUN_TEST(test_preference_mutation_and_lookup);
    RUN_TEST(test_preferences_round_trip_format2);
    RUN_TEST(test_preferences_invalid_and_missing);
    RUN_TEST(test_invalid_preferences_save_and_capacity);
    RUN_TEST(test_empty_save);
    return UNITY_END();
}
