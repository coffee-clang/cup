/*
 * Tests the scoped official installation policy and user preference overlay.
 */
#include "install_policy.h"
#include "tool_preferences.h"
#include "layout.h"
#include "registry.h"
#include "platform.h"
#include "system.h"
#include "text.h"
#include "unity.h"

#include <errno.h>
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
static char official_path[MAX_PATH_LEN];
static char preferences_path[MAX_PATH_LEN];
static int sync_parent_calls;
static CupError sync_parent_result;

/* Fixture lifecycle and local construction helpers. */

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

static const char *valid_policy(void) {
    return "format=1\n"
           "default.linux-x64.linux-x64.compiler=clang\n"
           "default.linux-x64.linux-x64.linker=lld\n"
           "default.linux-x64.windows-x64.compiler=gcc\n"
           "profile.minimal=compiler,linker\n"
           "profile.standard=compiler,linker,debugger,language-server\n"
           "toolchain.llvm=clang,lldb,lld,clang-format,clang-tidy,clangd\n"
           "toolchain.gnu=gcc,gdb,ld\n";
}

void setUp(void) {
    char template_path[] = "/tmp/cup-policy-unit-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(template_path));
    strcpy(root, template_path);
    path_join(official_path, sizeof(official_path), root, "install.cfg");
    path_join(preferences_path, sizeof(preferences_path), root, "preferences.txt");
    write_text(official_path, valid_policy());
    sync_parent_calls = 0;
    sync_parent_result = CUP_OK;
}

void tearDown(void) {
    unlink(preferences_path);
    unlink(official_path);
    rmdir(root);
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

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

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    return text_copy(buffer, size, official_path);
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
    struct stat info;
    if (path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (stat(path, &info) == 0) {
        *exists = 1;
        return CUP_OK;
    }
    if (errno == ENOENT) {
        *exists = 0;
        return CUP_OK;
    }
    return CUP_ERR_FILESYSTEM;
}

CupError system_create_temp_file(
    const char *directory, const char *prefix, char *path, size_t path_size, FILE **file) {
    int descriptor;
    int written;

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
    return file != NULL && fflush(file) == 0 && fsync(fileno(file)) == 0 ? CUP_OK
                                                                         : CUP_ERR_FILESYSTEM;
}

CupError system_replace_file(const char *source,
                             const char *destination,
                             SystemCommitState *state) {
    if (state == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *state = SYSTEM_COMMIT_NOT_APPLIED;
    if (rename(source, destination) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *state = SYSTEM_COMMIT_DURABLE;
    return CUP_OK;
}

CupError system_remove_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT ? CUP_OK : CUP_ERR_FILESYSTEM;
}

CupError system_sync_parent_directory(const char *path) {
    if (path == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    sync_parent_calls++;
    return sync_parent_result;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_policy_load(void) {
    InstallPolicy policy;
    const InstallDefault *official;
    const InstallNamedList *profile;
    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load_installed(&policy));
    TEST_ASSERT_EQUAL_INT(INSTALL_POLICY_SOURCE_INSTALLED, policy.source);
    official = install_policy_find_default(&policy, "linux-x64", "linux-x64", "compiler");
    TEST_ASSERT_NOT_NULL(official);
    TEST_ASSERT_EQUAL_STRING("clang", official->tool);
    official = install_policy_find_default(&policy, "linux-x64", "windows-x64", "compiler");
    TEST_ASSERT_NOT_NULL(official);
    TEST_ASSERT_EQUAL_STRING("gcc", official->tool);
    TEST_ASSERT_NULL(install_policy_find_default(&policy, "windows-x64", "linux-x64", "compiler"));
    profile = install_policy_find_profile(&policy, "standard");
    TEST_ASSERT_NOT_NULL(profile);
    TEST_ASSERT_EQUAL_size_t(4, profile->item_count);
    TEST_ASSERT_NOT_NULL(install_policy_find_toolchain(&policy, "gnu"));
}

static void assert_invalid_policy(const char *text) {
    InstallPolicy policy;
    write_text(official_path, text);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, install_policy_load_installed(&policy));
    TEST_ASSERT_EQUAL_INT(INSTALL_POLICY_SOURCE_NONE, policy.source);
}

static void test_policy_invalid(void) {
    assert_invalid_policy(
        "format=1\ndefault_preset=llvm\nprofile.minimal=compiler\ntoolchain.llvm=clang\n");
    assert_invalid_policy(
        "format=1\ndefault.linux-x64.linux-x64.compiler=clang\ndefault.linux-x64.linux-x64."
        "compiler=gcc\nprofile.minimal=compiler\ntoolchain.llvm=clang\n");
    assert_invalid_policy("format=1\ndefault.linux-x64.linux-x64.compiler=gdb\nprofile.minimal="
                          "compiler\ntoolchain.llvm=clang\n");
    assert_invalid_policy("format=1\ndefault.linux-x64.linux-x64.compiler=clang\nprofile.Mixed="
                          "compiler\ntoolchain.llvm=clang\n");
    assert_invalid_policy("format=1\ndefault.linux-x64.linux-x64.compiler=clang\nprofile.minimal="
                          "compiler\ntoolchain.bad=clang,gcc\n");
}

static void test_resolution_scope(void) {
    InstallPolicy policy;
    ToolPreferences preferences;
    ToolPreferenceSource source;
    char tool[MAX_IDENTIFIER_LEN];
    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load_installed(&policy));
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          tool_preferences_resolve(&policy,
                                                   &preferences,
                                                   "linux-x64",
                                                   "linux-x64",
                                                   "compiler",
                                                   tool,
                                                   sizeof(tool),
                                                   &source));
    TEST_ASSERT_EQUAL_STRING("clang", tool);
    TEST_ASSERT_EQUAL_INT(TOOL_PREFERENCE_OFFICIAL_DEFAULT, source);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          tool_preferences_resolve(&policy,
                                                   &preferences,
                                                   "linux-x64",
                                                   "linux-x64",
                                                   "compiler",
                                                   tool,
                                                   sizeof(tool),
                                                   &source));
    TEST_ASSERT_EQUAL_STRING("gcc", tool);
    TEST_ASSERT_EQUAL_INT(TOOL_PREFERENCE_USER, source);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          tool_preferences_resolve(&policy,
                                                   &preferences,
                                                   "linux-x64",
                                                   "windows-x64",
                                                   "compiler",
                                                   tool,
                                                   sizeof(tool),
                                                   &source));
    TEST_ASSERT_EQUAL_STRING("gcc", tool);
    TEST_ASSERT_EQUAL_INT(TOOL_PREFERENCE_OFFICIAL_DEFAULT, source);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          tool_preferences_resolve(&policy,
                                                   &preferences,
                                                   "windows-x64",
                                                   "windows-x64",
                                                   "analyzer",
                                                   tool,
                                                   sizeof(tool),
                                                   &source));
    TEST_ASSERT_EQUAL_INT(TOOL_PREFERENCE_NONE, source);
}

static void test_preference_mutation(void) {
    ToolPreferences preferences;
    int removed;
    size_t removed_count;
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "linker", "ld"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "windows-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_size_t(3, preferences.count);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        tool_preferences_reset(&preferences, "linux-x64", "linux-x64", "compiler", &removed));
    TEST_ASSERT_TRUE(removed);
    TEST_ASSERT_EQUAL_size_t(2, preferences.count);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        tool_preferences_reset_scope(&preferences, "linux-x64", "linux-x64", &removed_count));
    TEST_ASSERT_EQUAL_size_t(1, removed_count);
    TEST_ASSERT_EQUAL_size_t(1, preferences.count);
    TEST_ASSERT_EQUAL_STRING("windows-x64", preferences.items[0].scope.target_platform);
}

static void test_round_trip(void) {
    InstallPolicy policy;
    ToolPreferences preferences;
    ToolPreferences loaded;
    char *text;
    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load_installed(&policy));
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "windows-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "linker", "ld"));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, tool_preferences_set(&preferences, "linux-x64", "linux-x64", "compiler", "gcc"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_save(&policy, &preferences));
    text = read_text(preferences_path);
    TEST_ASSERT_EQUAL_STRING("format=1\n"
                             "preferred.linux-x64.linux-x64.compiler=gcc\n"
                             "preferred.linux-x64.linux-x64.linker=ld\n"
                             "preferred.linux-x64.windows-x64.compiler=gcc\n",
                             text);
    free(text);
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_load(&policy, &loaded));
    TEST_ASSERT_EQUAL_size_t(3, loaded.count);
}

static void test_preferences_invalid(void) {
    InstallPolicy policy;
    ToolPreferences preferences;
    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load_installed(&policy));
    write_text(preferences_path, "format=1\npreset=llvm\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&policy, &preferences));
    write_text(preferences_path,
               "format=1\npreferred.linux-x64.linux-x64.compiler=clang\n"
               "preferred.linux-x64.linux-x64.compiler=gcc\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&policy, &preferences));
    write_text(preferences_path, "format=1\npreferred.linux-x64.linux-x64.compiler=gdb\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, tool_preferences_load(&policy, &preferences));
}

static void test_empty_save(void) {
    InstallPolicy policy;
    ToolPreferences preferences;
    int exists = 0;
    TEST_ASSERT_EQUAL_INT(CUP_OK, install_policy_load_installed(&policy));
    write_text(preferences_path, "format=1\n");
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(CUP_OK, tool_preferences_save(&policy, &preferences));
    TEST_ASSERT_EQUAL_INT(CUP_OK, system_path_exists(preferences_path, &exists));
    TEST_ASSERT_FALSE(exists);
    TEST_ASSERT_EQUAL_INT(1, sync_parent_calls);
}

static void test_argument_contracts(void) {
    InstallPolicy policy;
    ToolPreferences preferences;
    ToolPreferenceSource source;
    char tool[2];
    int removed;
    size_t count;
    install_policy_init(&policy);
    tool_preferences_init(&preferences);
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        install_policy_load_path(NULL, official_path, INSTALL_POLICY_SOURCE_INSTALLED));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        tool_preferences_set(NULL, "linux-x64", "linux-x64", "compiler", "clang"));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        tool_preferences_reset(&preferences, "linux-x64", "linux-x64", "compiler", NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        tool_preferences_reset_scope(&preferences, "linux-x64", "linux-x64", NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        tool_preferences_resolve(
            NULL, &preferences, "linux-x64", "linux-x64", "compiler", tool, sizeof(tool), &source));
    (void)removed;
    (void)count;
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_policy_load);
    RUN_TEST(test_policy_invalid);
    RUN_TEST(test_resolution_scope);
    RUN_TEST(test_preference_mutation);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_preferences_invalid);
    RUN_TEST(test_empty_save);
    RUN_TEST(test_argument_contracts);
    return UNITY_END();
}
