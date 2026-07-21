/*
 * Tests scoped config view, set and reset command behavior.
 */
#include "command_context.h"
#include "commands.h"
#include "install_policy.h"
#include "tool_preferences.h"
#include "registry.h"
#include "text.h"
#include "unity.h"

#include <string.h>

static ToolPreferences loaded;
static ToolPreferences saved;
static InstallDefault official;
static int save_calls;
static int load_calls;
static int operational_calls;
static int read_only_calls;
static int end_calls;
static CupError begin_result;
static CupError policy_result;
static CupError load_result;
static CupError save_result;
static CupError resolve_result;

void setUp(void) {
    memset(&loaded, 0, sizeof(loaded));
    memset(&saved, 0, sizeof(saved));
    memset(&official, 0, sizeof(official));
    strcpy(official.scope.component, "compiler");
    strcpy(official.scope.host_platform, "linux-x64");
    strcpy(official.scope.target_platform, "linux-x64");
    strcpy(official.tool, "clang");
    save_calls = load_calls = operational_calls = read_only_calls = end_calls = 0;
    begin_result = policy_result = load_result = save_result = resolve_result = CUP_OK;
}
void tearDown(void) {
}

static CupError begin_common(CommandContext *context, const char *target) {
    memset(context, 0, sizeof(*context));
    strcpy(context->host_platform, "linux-x64");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          text_copy_lower_ascii(context->target_platform,
                                                sizeof(context->target_platform),
                                                target == NULL ? "linux-x64" : target));
    return begin_result;
}
CupError command_context_begin(CommandContext *context, const char *target, SystemLockMode mode) {
    (void)mode;
    operational_calls++;
    return begin_common(context, target);
}
CupError command_context_begin_read_only(CommandContext *context, const char *target) {
    read_only_calls++;
    return begin_common(context, target);
}
void command_context_end(CommandContext *context) {
    (void)context;
    end_calls++;
}
void install_policy_init(InstallPolicy *policy) {
    memset(policy, 0, sizeof(*policy));
}
CupError install_policy_load(InstallPolicy *policy) {
    if (policy_result != CUP_OK)
        return policy_result;
    policy->profile_count = 1;
    strcpy(policy->profiles[0].name, "minimal");
    policy->profiles[0].item_count = 2;
    strcpy(policy->profiles[0].items[0], "compiler");
    strcpy(policy->profiles[0].items[1], "linker");
    policy->toolchain_count = 1;
    strcpy(policy->toolchains[0].name, "llvm");
    policy->toolchains[0].item_count = 1;
    strcpy(policy->toolchains[0].items[0], "clang");
    return CUP_OK;
}
const InstallDefault *install_policy_find_default(const InstallPolicy *policy,
                                                  const char *host,
                                                  const char *target,
                                                  const char *component) {
    (void)policy;
    return strcmp(host, "linux-x64") == 0 && strcmp(target, "linux-x64") == 0 &&
                   strcmp(component, "compiler") == 0
               ? &official
               : NULL;
}
void tool_preferences_init(ToolPreferences *preferences) {
    memset(preferences, 0, sizeof(*preferences));
}
CupError tool_preferences_load(const InstallPolicy *policy, ToolPreferences *preferences) {
    (void)policy;
    load_calls++;
    if (load_result == CUP_OK)
        *preferences = loaded;
    return load_result;
}
CupError tool_preferences_save(const InstallPolicy *policy, const ToolPreferences *preferences) {
    (void)policy;
    save_calls++;
    saved = *preferences;
    return save_result;
}
CupError tool_preferences_set(ToolPreferences *preferences,
                              const char *host,
                              const char *target,
                              const char *component,
                              const char *tool) {
    if (registry_validate_tool(component, tool) != CUP_OK)
        return CUP_ERR_INVALID_INPUT;
    preferences->count = 1;
    strcpy(preferences->items[0].scope.component, component);
    strcpy(preferences->items[0].scope.host_platform, host);
    strcpy(preferences->items[0].scope.target_platform, target);
    strcpy(preferences->items[0].tool, tool);
    return CUP_OK;
}
CupError tool_preferences_reset(ToolPreferences *preferences,
                                const char *host,
                                const char *target,
                                const char *component,
                                int *removed) {
    (void)host;
    (void)target;
    *removed =
        preferences->count != 0 && strcmp(preferences->items[0].scope.component, component) == 0;
    if (*removed)
        preferences->count = 0;
    return CUP_OK;
}
CupError tool_preferences_reset_scope(ToolPreferences *preferences,
                                      const char *host,
                                      const char *target,
                                      size_t *removed_count) {
    (void)host;
    (void)target;
    *removed_count = preferences->count;
    preferences->count = 0;
    return CUP_OK;
}
CupError tool_preferences_resolve(const InstallPolicy *policy,
                                  const ToolPreferences *preferences,
                                  const char *host,
                                  const char *target,
                                  const char *component,
                                  char *tool,
                                  size_t tool_size,
                                  ToolPreferenceSource *source) {
    (void)policy;
    (void)host;
    (void)target;
    if (resolve_result != CUP_OK)
        return resolve_result;
    if (preferences->count != 0 && strcmp(preferences->items[0].scope.component, component) == 0) {
        *source = TOOL_PREFERENCE_USER;
        return text_copy(tool, tool_size, preferences->items[0].tool);
    }
    if (strcmp(component, "compiler") == 0) {
        *source = TOOL_PREFERENCE_OFFICIAL_DEFAULT;
        return text_copy(tool, tool_size, "clang");
    }
    *source = TOOL_PREFERENCE_NONE;
    tool[0] = '\0';
    return CUP_ERR_NOT_AVAILABLE;
}

static void seed_preference(const char *target, const char *component, const char *tool) {
    loaded.count = 1;
    strcpy(loaded.items[0].scope.host_platform, "linux-x64");
    strcpy(loaded.items[0].scope.target_platform, target);
    strcpy(loaded.items[0].scope.component, component);
    strcpy(loaded.items[0].tool, tool);
}
static void test_view_is_read_only(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_config(NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, read_only_calls);
    TEST_ASSERT_EQUAL_INT(0, operational_calls);
    TEST_ASSERT_EQUAL_INT(1, load_calls);
    TEST_ASSERT_EQUAL_INT(0, save_calls);
    TEST_ASSERT_EQUAL_INT(1, end_calls);
}
static void test_set_scope(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_config("SET", "COMPILER", "GCC", "WINDOWS-X64"));
    TEST_ASSERT_EQUAL_INT(1, operational_calls);
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_size_t(1, saved.count);
    TEST_ASSERT_EQUAL_STRING("linux-x64", saved.items[0].scope.host_platform);
    TEST_ASSERT_EQUAL_STRING("windows-x64", saved.items[0].scope.target_platform);
    TEST_ASSERT_EQUAL_STRING("compiler", saved.items[0].scope.component);
    TEST_ASSERT_EQUAL_STRING("gcc", saved.items[0].tool);
}
static void test_reset_component(void) {
    seed_preference("linux-x64", "compiler", "gcc");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_config("reset", "compiler", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_size_t(0, saved.count);
}
static void test_reset_scope(void) {
    seed_preference("linux-x64", "compiler", "gcc");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_config("reset", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_size_t(0, saved.count);
}
static void test_invalid_args(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_config("preset", "llvm", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_config("set", "compiler", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_config("reset", "compiler", "extra", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT,
                          command_config("reset", "unknown", NULL, NULL));
}
static void test_error_propagation(void) {
    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_config(NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, end_calls);
    begin_result = CUP_OK;
    policy_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_config("set", "compiler", "clang", NULL));
    policy_result = CUP_OK;
    load_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_config("set", "compiler", "clang", NULL));
    load_result = CUP_OK;
    save_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_config("set", "compiler", "clang", NULL));
}
static void test_view_error(void) {
    resolve_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_config(NULL, NULL, NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_view_is_read_only);
    RUN_TEST(test_set_scope);
    RUN_TEST(test_reset_component);
    RUN_TEST(test_reset_scope);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_error_propagation);
    RUN_TEST(test_view_error);
    return UNITY_END();
}
