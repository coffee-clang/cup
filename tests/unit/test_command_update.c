/*
 * Test focus: Exercises update-plan selection, scope deduplication, default compare-and-swap
 * inputs and per-scope outcomes.
 */

#include "command_context.h"
#include "commands.h"
#include "package_selector.h"
#include "package_install.h"
#include "state.h"
#include "system.h"
#include "unity.h"

#include <string.h>

#define MAX_SCOPE_CALLS 8

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
    char expected_active[MAX_SELECTOR_LEN];
} ScopeCall;

static CupState scenario_state;
static CupError begin_result;
static CupError load_result;
static CupError scope_results[MAX_SCOPE_CALLS];
static int scope_installed[MAX_SCOPE_CALLS];
static int scope_active_moved[MAX_SCOPE_CALLS];
static ScopeCall scope_calls[MAX_SCOPE_CALLS];
static size_t scope_call_count;
static int context_end_calls;
static CupError cup_update_result;
static int cup_update_calls;

static void reset_scenario(void) {
    size_t i;

    memset(&scenario_state, 0, sizeof(scenario_state));
    memset(scope_calls, 0, sizeof(scope_calls));
    begin_result = CUP_OK;
    load_result = CUP_OK;
    scope_call_count = 0;
    context_end_calls = 0;
    cup_update_result = CUP_OK;
    cup_update_calls = 0;
    for (i = 0; i < MAX_SCOPE_CALLS; ++i) {
        scope_results[i] = CUP_OK;
        scope_installed[i] = 0;
        scope_active_moved[i] = 0;
    }
}

static void set_identity(PackageIdentity *item,
                         const char *component,
                         const char *host,
                         const char *target,
                         const char *entry) {
    char tool[MAX_IDENTIFIER_LEN] = "";
    char version[MAX_IDENTIFIER_LEN] = "";

    memset(item, 0, sizeof(*item));
    strcpy(item->component, component);
    strcpy(item->host_platform, host);
    strcpy(item->target_platform, target);
    if (package_selector_parse_parts(entry, tool, sizeof(tool), version, sizeof(version)) ==
        CUP_OK) {
        strcpy(item->tool, tool);
        strcpy(item->version, version);
    }
}

static void add_installed(const char *component,
                          const char *host,
                          const char *target,
                          const char *entry) {
    PackageIdentity *item = &scenario_state.installed[scenario_state.installed_count++];

    set_identity(item, component, host, target, entry);
}

static void add_active(const char *component,
                       const char *host,
                       const char *target,
                       const char *entry) {
    PackageIdentity *item = &scenario_state.active[scenario_state.active_count++];

    set_identity(item, component, host, target, entry);
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)target_override;
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_SHARED, mode);
    if (begin_result != CUP_OK) {
        return begin_result;
    }

    memset(context, 0, sizeof(*context));
    context->state = scenario_state;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, "linux-x64");
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context_end_calls++;
}

CupError command_context_load_state(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_result;
}

const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope) {
    size_t i;

    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(scope);
    for (i = 0; i < state->active_count; ++i) {
        const PackageIdentity *item = &state->active[i];
        if (strcmp(item->component, scope->component) == 0 &&
            strcmp(item->host_platform, scope->host_platform) == 0 &&
            strcmp(item->target_platform, scope->target_platform) == 0) {
            return item;
        }
    }
    return NULL;
}

CupError package_identity_validate(const PackageIdentity *identity) {
    if (identity == NULL || identity->component[0] == '\0' || identity->tool[0] == '\0' ||
        identity->host_platform[0] == '\0' || identity->target_platform[0] == '\0' ||
        identity->version[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    return CUP_OK;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    if (package_identity_validate(identity) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform) {
    TEST_ASSERT_NOT_NULL(scope);
    memset(scope, 0, sizeof(*scope));
    strcpy(scope->component, component);
    strcpy(scope->host_platform, host_platform);
    strcpy(scope->target_platform, target_platform);
    return CUP_OK;
}

CupError package_install_update_scope(const char *component,
                                      const char *tool,
                                      const char *target_override,
                                      const char *expected_active,
                                      int *installed,
                                      int *active_moved) {
    size_t index = scope_call_count++;
    ScopeCall *call;

    TEST_ASSERT_TRUE(index < MAX_SCOPE_CALLS);
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_NOT_NULL(target_override);
    TEST_ASSERT_NOT_NULL(expected_active);
    TEST_ASSERT_NOT_NULL(installed);
    TEST_ASSERT_NOT_NULL(active_moved);

    call = &scope_calls[index];
    strcpy(call->component, component);
    strcpy(call->tool, tool);
    strcpy(call->target, target_override);
    strcpy(call->expected_active, expected_active);
    *installed = scope_installed[index];
    *active_moved = scope_active_moved[index];
    return scope_results[index];
}

CupError command_update_cup(void) {
    cup_update_calls++;
    return cup_update_result;
}

static void assert_scope(size_t index,
                         const char *component,
                         const char *tool,
                         const char *target,
                         const char *expected_active) {
    TEST_ASSERT_TRUE(index < scope_call_count);
    TEST_ASSERT_EQUAL_STRING(component, scope_calls[index].component);
    TEST_ASSERT_EQUAL_STRING(tool, scope_calls[index].tool);
    TEST_ASSERT_EQUAL_STRING(target, scope_calls[index].target);
    TEST_ASSERT_EQUAL_STRING(expected_active, scope_calls[index].expected_active);
}

static void test_global_selector(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update(NULL));
    TEST_ASSERT_EQUAL_INT(0, cup_update_calls);
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update(""));
    TEST_ASSERT_EQUAL_INT(0, cup_update_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("CuP"));
    TEST_ASSERT_EQUAL_INT(1, cup_update_calls);
    TEST_ASSERT_EQUAL_INT(0, context_end_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL, command_update("unknown"));
    TEST_ASSERT_EQUAL_INT(0, (int)scope_call_count);
    TEST_ASSERT_EQUAL_INT(0, cup_update_calls);
    TEST_ASSERT_EQUAL_INT(0, context_end_calls);
}

static void test_context_failures(void) {
    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);

    reset_scenario();
    load_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
}

static void test_empty_plan(void) {
    add_installed("compiler", "macos-x64", "linux-x64", "clang@1.0.0");
    add_installed("debugger", "linux-x64", "linux-x64", "gdb@1.0.0");
    add_installed("compiler", "linux-x64", "linux-x64", "gcc@1.0.0");

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(0, (int)(scope_call_count));
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
}

static void test_tool_plan(void) {
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0.0");
    add_installed("compiler", "linux-x64", "windows-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "linux-x64", "gcc@1.0.0");
    add_installed("compiler", "macos-x64", "linux-x64", "clang@3.0.0");
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_active("compiler", "linux-x64", "windows-x64", "gcc@1.0.0");

    scope_installed[0] = 1;
    scope_active_moved[0] = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(2, (int)(scope_call_count));
    assert_scope(0, "compiler", "clang", "linux-x64", "clang@1.0.0");
    assert_scope(1, "compiler", "clang", "windows-x64", "");
}

static void test_component_plan(void) {
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "linux-x64", "gcc@1.0.0");
    add_installed("compiler", "linux-x64", "windows-x64", "clang@1.0.0");
    add_active("compiler", "linux-x64", "linux-x64", "gcc@1.0.0");

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("compiler"));
    TEST_ASSERT_EQUAL_INT(3, (int)(scope_call_count));
    assert_scope(0, "compiler", "clang", "linux-x64", "");
    assert_scope(1, "compiler", "gcc", "linux-x64", "gcc@1.0.0");
    assert_scope(2, "compiler", "clang", "windows-x64", "");
}

static void test_global_excludes_cup(void) {
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_installed("debugger", "linux-x64", "linux-x64", "gdb@1.0.0");

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update(NULL));
    TEST_ASSERT_EQUAL_INT(2, (int)scope_call_count);
    TEST_ASSERT_EQUAL_INT(0, cup_update_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("cup"));
    TEST_ASSERT_EQUAL_INT(0, (int)scope_call_count);
    TEST_ASSERT_EQUAL_INT(1, cup_update_calls);
}

static void test_invalid_state(void) {
    add_installed("compiler", "linux-x64", "linux-x64", "broken");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_update("compiler"));
    TEST_ASSERT_EQUAL_INT(0, (int)(scope_call_count));

    reset_scenario();
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_active("compiler", "linux-x64", "linux-x64", "broken");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(0, (int)(scope_call_count));
}

static void test_scope_outcomes(void) {
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "linux-x64", "gcc@1.0.0");
    add_installed("compiler", "linux-x64", "windows-x64", "clang@1.0.0");

    scope_installed[0] = 1;
    scope_active_moved[0] = 1;
    scope_results[1] = CUP_ERR_NOT_INSTALLED;
    scope_results[2] = CUP_ERR_FETCH;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update("compiler"));
    TEST_ASSERT_EQUAL_INT(3, (int)(scope_call_count));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_global_selector);
    RUN_TEST(test_context_failures);
    RUN_TEST(test_empty_plan);
    RUN_TEST(test_tool_plan);
    RUN_TEST(test_component_plan);
    RUN_TEST(test_global_excludes_cup);
    RUN_TEST(test_invalid_state);
    RUN_TEST(test_scope_outcomes);
    return UNITY_END();
}
