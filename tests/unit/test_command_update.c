/* Exercises the frozen package-update semantics: local precheck, one required refresh, semantic
 * family reference selection, no-downgrade, revision updates and sequential execution. */

#include "catalog_refresh.h"
#include "command_context.h"
#include "commands.h"
#include "package_artifact.h"
#include "package_catalog.h"
#include "package_install.h"
#include "package_selector.h"
#include "self_update.h"
#include "state.h"
#include "system.h"
#include "unity.h"

#include <string.h>

#define MAX_SCENARIO_INSTALLED 20
#define MAX_FAMILIES 12
#define MAX_CALLS 12

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
    char version[MAX_IDENTIFIER_LEN];
    CupError stable_result;
    int reference_advertised;
} StableScenario;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
    char target_version[MAX_IDENTIFIER_LEN];
    char expected_reference[MAX_SELECTOR_LEN];
    char expected_default[MAX_SELECTOR_LEN];
} UpdateCall;

static CupState scenario_state;
static PackageIdentity scenario_installed[MAX_SCENARIO_INSTALLED];
static StableScenario stable_scenarios[MAX_FAMILIES];
static size_t stable_scenario_count;
static UpdateCall update_calls[MAX_CALLS];
static size_t update_call_count;
static CupError update_results[MAX_CALLS];
static int update_installed[MAX_CALLS];
static int update_default_moved[MAX_CALLS];
static CupError begin_result;
static CupError load_state_result;
static CupError load_catalog_result;
static CupError refresh_result;
static int refresh_calls;
static CatalogRefreshDiagnostics refresh_diagnostics;
static int begin_calls;
static int initialize_calls;
static int end_calls;
static int load_catalog_calls;
static CupError seed_catalog_result;
static int seed_catalog_calls;
static CupError self_update_result;
static int self_update_calls;

static void reset_scenario(void) {
    size_t i;

    memset(&scenario_state, 0, sizeof(scenario_state));
    memset(scenario_installed, 0, sizeof(scenario_installed));
    scenario_state.installed = scenario_installed;
    scenario_state.installed_capacity = MAX_SCENARIO_INSTALLED;
    memset(stable_scenarios, 0, sizeof(stable_scenarios));
    memset(update_calls, 0, sizeof(update_calls));
    stable_scenario_count = 0;
    update_call_count = 0;
    begin_result = CUP_OK;
    load_state_result = CUP_OK;
    load_catalog_result = CUP_OK;
    refresh_result = CUP_OK;
    refresh_calls = 0;
    refresh_diagnostics = CATALOG_REFRESH_QUIET;
    begin_calls = 0;
    initialize_calls = 0;
    end_calls = 0;
    load_catalog_calls = 0;
    seed_catalog_result = CUP_OK;
    seed_catalog_calls = 0;
    self_update_result = CUP_OK;
    self_update_calls = 0;
    for (i = 0; i < MAX_CALLS; ++i) {
        update_results[i] = CUP_OK;
        update_installed[i] = 0;
        update_default_moved[i] = 0;
    }
}

static void set_identity(PackageIdentity *identity,
                         const char *component,
                         const char *target,
                         const char *selector) {
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];

    memset(identity, 0, sizeof(*identity));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_selector_parse_parts(selector, tool, sizeof(tool), version, sizeof(version)));
    strcpy(identity->component, component);
    strcpy(identity->tool, tool);
    strcpy(identity->host_platform, "linux-x64");
    strcpy(identity->target_platform, target);
    strcpy(identity->version, version);
}

static void add_installed(const char *component, const char *target, const char *selector) {
    TEST_ASSERT_TRUE(scenario_state.installed_count < MAX_SCENARIO_INSTALLED);
    set_identity(&scenario_state.installed[scenario_state.installed_count++],
                 component,
                 target,
                 selector);
}

static void add_default(const char *component, const char *target, const char *selector) {
    TEST_ASSERT_TRUE(scenario_state.default_count < MAX_STATE_DEFAULTS);
    set_identity(&scenario_state.defaults[scenario_state.default_count++],
                 component,
                 target,
                 selector);
}

static void add_stable(const char *component,
                       const char *tool,
                       const char *target,
                       const char *version,
                       int reference_advertised) {
    StableScenario *scenario;

    TEST_ASSERT_TRUE(stable_scenario_count < MAX_FAMILIES);
    scenario = &stable_scenarios[stable_scenario_count++];
    strcpy(scenario->component, component);
    strcpy(scenario->tool, tool);
    strcpy(scenario->target, target);
    strcpy(scenario->version, version);
    scenario->stable_result = CUP_OK;
    scenario->reference_advertised = reference_advertised;
}

static void add_no_stable(const char *component,
                          const char *tool,
                          const char *target,
                          int reference_advertised) {
    StableScenario *scenario;

    TEST_ASSERT_TRUE(stable_scenario_count < MAX_FAMILIES);
    scenario = &stable_scenarios[stable_scenario_count++];
    strcpy(scenario->component, component);
    strcpy(scenario->tool, tool);
    strcpy(scenario->target, target);
    scenario->stable_result = CUP_ERR_NOT_AVAILABLE;
    scenario->reference_advertised = reference_advertised;
}

static StableScenario *find_stable(const char *component, const char *tool, const char *target) {
    size_t i;

    for (i = 0; i < stable_scenario_count; ++i) {
        if (strcmp(stable_scenarios[i].component, component) == 0 &&
            strcmp(stable_scenarios[i].tool, tool) == 0 &&
            strcmp(stable_scenarios[i].target, target) == 0) {
            return &stable_scenarios[i];
        }
    }
    return NULL;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)target_override;
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_SHARED, mode);
    begin_calls++;
    if (begin_result != CUP_OK) {
        return begin_result;
    }
    memset(context, 0, sizeof(*context));
    context->state = scenario_state;
    context->runtime_available = 1;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, "linux-x64");
    return CUP_OK;
}

CupError command_context_begin_initialize(CommandContext *context,
                                          const char *target_override,
                                          SystemLockMode mode) {
    (void)target_override;
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    initialize_calls++;
    if (begin_result != CUP_OK) {
        return begin_result;
    }
    memset(context, 0, sizeof(*context));
    context->state = scenario_state;
    context->runtime_available = 1;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, "linux-x64");
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    end_calls++;
    memset(context, 0, sizeof(*context));
}

CupError command_context_load_state(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_state_result;
}

CupError command_context_load_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    load_catalog_calls++;
    if (load_catalog_result == CUP_OK) {
        context->has_catalog = 1;
    }
    return load_catalog_result;
}

CupError package_catalog_seed_runtime(void) {
    seed_catalog_calls++;
    return seed_catalog_result;
}

const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope) {
    size_t i;

    for (i = 0; i < state->default_count; ++i) {
        const PackageIdentity *identity = &state->defaults[i];
        if (strcmp(identity->component, scope->component) == 0 &&
            strcmp(identity->host_platform, scope->host_platform) == 0 &&
            strcmp(identity->target_platform, scope->target_platform) == 0) {
            return identity;
        }
    }
    return NULL;
}

CupError state_get_tool_reference(const CupState *state,
                                  const PackageScope *scope,
                                  const char *tool,
                                  PackageIdentity *reference,
                                  int *reference_is_default) {
    const PackageIdentity *current_default;
    const PackageIdentity *best = NULL;
    size_t i;

    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(scope);
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_NOT_NULL(reference);
    TEST_ASSERT_NOT_NULL(reference_is_default);
    *reference_is_default = 0;
    current_default = state_get_default(state, scope);
    if (current_default != NULL && strcmp(current_default->tool, tool) == 0) {
        *reference = *current_default;
        *reference_is_default = 1;
        return CUP_OK;
    }
    for (i = 0; i < state->installed_count; ++i) {
        const PackageIdentity *candidate = &state->installed[i];
        int compared;

        if (strcmp(candidate->component, scope->component) != 0 ||
            strcmp(candidate->host_platform, scope->host_platform) != 0 ||
            strcmp(candidate->target_platform, scope->target_platform) != 0 ||
            strcmp(candidate->tool, tool) != 0) {
            continue;
        }
        if (best == NULL) {
            best = candidate;
            continue;
        }
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              package_release_compare(candidate->version, best->version, &compared));
        if (compared > 0) {
            best = candidate;
        }
    }
    if (best == NULL) {
        return CUP_ERR_NOT_INSTALLED;
    }
    *reference = *best;
    return CUP_OK;
}


CupError package_identity_validate(const PackageIdentity *identity, FILE *diagnostics) {
    (void)diagnostics;
    return identity != NULL && identity->component[0] != '\0' && identity->tool[0] != '\0' &&
                   identity->host_platform[0] != '\0' && identity->target_platform[0] != '\0' &&
                   identity->version[0] != '\0'
               ? CUP_OK
               : CUP_ERR_INVALID_INPUT;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    if (package_identity_validate(identity, NULL) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform) {
    memset(scope, 0, sizeof(*scope));
    strcpy(scope->component, component);
    strcpy(scope->host_platform, host_platform);
    strcpy(scope->target_platform, target_platform);
    return CUP_OK;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available) {
    StableScenario *scenario;
    (void)catalog;
    (void)host_platform;
    (void)version;
    TEST_ASSERT_NOT_NULL(is_available);
    scenario = find_stable(component, tool, target_platform);
    *is_available = scenario != NULL ? scenario->reference_advertised : 0;
    return CUP_OK;
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform) {
    StableScenario *scenario;
    (void)catalog;
    (void)host_platform;
    scenario = find_stable(component, tool, target_platform);
    if (scenario == NULL) {
        return CUP_ERR_NOT_AVAILABLE;
    }
    if (scenario->stable_result != CUP_OK) {
        return scenario->stable_result;
    }
    if (strlen(scenario->version) + 1 > size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    strcpy(buffer, scenario->version);
    return CUP_OK;
}

CupError package_artifact_spec_resolve_stable(PackageArtifactSpec *spec,
                                              const PackageCatalog *catalog,
                                              const char *component,
                                              const char *tool,
                                              const char *host_platform,
                                              const char *target_platform) {
    StableScenario *scenario;
    (void)catalog;
    scenario = find_stable(component, tool, target_platform);
    TEST_ASSERT_NOT_NULL(scenario);
    TEST_ASSERT_EQUAL_INT(CUP_OK, scenario->stable_result);
    memset(spec, 0, sizeof(*spec));
    strcpy(spec->identity.component, component);
    strcpy(spec->identity.tool, tool);
    strcpy(spec->identity.host_platform, host_platform);
    strcpy(spec->identity.target_platform, target_platform);
    strcpy(spec->identity.version, scenario->version);
    spec->format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
    return CUP_OK;
}

CupError package_install_update_artifact(const PackageArtifactSpec *spec,
                                         const PackageIdentity *expected_reference,
                                         const PackageIdentity *expected_default,
                                         int *installed,
                                         int *default_moved) {
    size_t index = update_call_count++;
    UpdateCall *call;

    TEST_ASSERT_TRUE(index < MAX_CALLS);
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_NOT_NULL(expected_reference);
    TEST_ASSERT_NOT_NULL(installed);
    TEST_ASSERT_NOT_NULL(default_moved);
    call = &update_calls[index];
    strcpy(call->component, spec->identity.component);
    strcpy(call->tool, spec->identity.tool);
    strcpy(call->target, spec->identity.target_platform);
    strcpy(call->target_version, spec->identity.version);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_identity_format_selector(expected_reference,
                                                           call->expected_reference,
                                                           sizeof(call->expected_reference)));
    if (expected_default != NULL) {
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              package_identity_format_selector(expected_default,
                                                               call->expected_default,
                                                               sizeof(call->expected_default)));
    }
    *installed = update_installed[index];
    *default_moved = update_default_moved[index];
    return update_results[index];
}

CupError catalog_refresh_existing(int *updated, CatalogRefreshDiagnostics diagnostics) {
    refresh_diagnostics = diagnostics;
    TEST_ASSERT_NOT_NULL(updated);
    refresh_calls++;
    *updated = refresh_result == CUP_OK;
    return refresh_result;
}

CupError self_update_start(void) {
    self_update_calls++;
    return self_update_result;
}

static void assert_call(size_t index,
                        const char *component,
                        const char *tool,
                        const char *target,
                        const char *target_version,
                        const char *reference,
                        const char *expected_default) {
    TEST_ASSERT_TRUE(index < update_call_count);
    TEST_ASSERT_EQUAL_STRING(component, update_calls[index].component);
    TEST_ASSERT_EQUAL_STRING(tool, update_calls[index].tool);
    TEST_ASSERT_EQUAL_STRING(target, update_calls[index].target);
    TEST_ASSERT_EQUAL_STRING(target_version, update_calls[index].target_version);
    TEST_ASSERT_EQUAL_STRING(reference, update_calls[index].expected_reference);
    TEST_ASSERT_EQUAL_STRING(expected_default, update_calls[index].expected_default);
}

static void test_reserved_update_selectors(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("cup"));
    TEST_ASSERT_EQUAL_INT(1, self_update_calls);
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("catalog"));
    TEST_ASSERT_EQUAL_INT(1, initialize_calls);
    TEST_ASSERT_EQUAL_INT(1, seed_catalog_calls);
    TEST_ASSERT_EQUAL_INT(1, load_catalog_calls);
    TEST_ASSERT_EQUAL_INT(1, refresh_calls);
    TEST_ASSERT_EQUAL_INT(CATALOG_REFRESH_REPORT_ERRORS, refresh_diagnostics);
    TEST_ASSERT_EQUAL_INT(0, update_call_count);

    reset_scenario();
    seed_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_update("catalog"));
    TEST_ASSERT_EQUAL_INT(1, initialize_calls);
    TEST_ASSERT_EQUAL_INT(1, seed_catalog_calls);
    TEST_ASSERT_EQUAL_INT(0, load_catalog_calls);
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);
}

static void test_empty_update_avoids_network(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update(NULL));
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);
    TEST_ASSERT_EQUAL_INT(1, begin_calls);
    TEST_ASSERT_EQUAL_INT(0, load_catalog_calls);

    reset_scenario();
    add_installed("debugger", "linux-x64", "gdb@1.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);
    TEST_ASSERT_EQUAL_INT(0, update_call_count);
}

static void test_refresh_is_required_before_planning(void) {
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    add_stable("compiler", "clang", "linux-x64", "2.0.0", 1);
    refresh_result = CUP_ERR_FETCH;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, refresh_calls);
    TEST_ASSERT_EQUAL_INT(CATALOG_REFRESH_REPORT_ERRORS, refresh_diagnostics);
    TEST_ASSERT_EQUAL_INT(1, begin_calls);
    TEST_ASSERT_EQUAL_INT(0, load_catalog_calls);
    TEST_ASSERT_EQUAL_INT(0, update_call_count);
}

static void test_reference_uses_same_tool_default_otherwise_max(void) {
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "clang@3.0.0");
    add_installed("compiler", "windows-x64", "clang@1.0.0");
    add_installed("compiler", "windows-x64", "clang@1.5.0");
    add_default("compiler", "linux-x64", "clang@1.0.0");
    add_default("compiler", "windows-x64", "gcc@9.0.0");
    add_stable("compiler", "clang", "linux-x64", "2.0.0", 1);
    add_stable("compiler", "clang", "windows-x64", "2.0.0", 1);
    update_installed[0] = 1;
    update_default_moved[0] = 1;
    update_installed[1] = 1;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, refresh_calls);
    TEST_ASSERT_EQUAL_INT(2, begin_calls);
    TEST_ASSERT_EQUAL_INT(1, load_catalog_calls);
    TEST_ASSERT_EQUAL_INT(2, update_call_count);
    assert_call(0, "compiler", "clang", "linux-x64", "2.0.0", "clang@1.0.0", "clang@1.0.0");
    assert_call(1, "compiler", "clang", "windows-x64", "2.0.0", "clang@1.5.0", "");
}

static void test_no_stable_and_ahead_are_skipped(void) {
    add_installed("compiler", "linux-x64", "clang@3.0.0");
    add_installed("compiler", "linux-x64", "gcc@5.0.0");
    add_no_stable("compiler", "clang", "linux-x64", 0);
    add_stable("compiler", "gcc", "linux-x64", "4.0.0", 1);

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("compiler"));
    TEST_ASSERT_EQUAL_INT(1, refresh_calls);
    TEST_ASSERT_EQUAL_INT(0, update_call_count);
}

static void test_equal_stable_reuses_installed_package(void) {
    add_installed("compiler", "linux-x64", "clang@2.0.0");
    add_stable("compiler", "clang", "linux-x64", "2.0.0", 1);

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, update_call_count);
    assert_call(0, "compiler", "clang", "linux-x64", "2.0.0", "clang@2.0.0", "");
}

static void test_revision_is_normal_newer_version(void) {
    add_installed("compiler", "linux-x64", "clang@23.2.0");
    add_default("compiler", "linux-x64", "clang@23.2.0");
    add_stable("compiler", "clang", "linux-x64", "23.2.0-rev1", 1);

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, update_call_count);
    assert_call(0,
                "compiler",
                "clang",
                "linux-x64",
                "23.2.0-rev1",
                "clang@23.2.0",
                "clang@23.2.0");
}

static void test_sequential_failure_stops_later_families(void) {
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    add_installed("compiler", "linux-x64", "gcc@1.0.0");
    add_stable("compiler", "clang", "linux-x64", "2.0.0", 1);
    add_stable("compiler", "gcc", "linux-x64", "2.0.0", 1);
    update_installed[0] = 1;
    update_results[1] = CUP_ERR_FETCH;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_update("compiler"));
    TEST_ASSERT_EQUAL_INT(2, update_call_count);
}

static void test_context_and_catalog_failures(void) {
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);

    reset_scenario();
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(0, refresh_calls);

    reset_scenario();
    add_installed("compiler", "linux-x64", "clang@1.0.0");
    add_stable("compiler", "clang", "linux-x64", "2.0.0", 1);
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_update("clang"));
    TEST_ASSERT_EQUAL_INT(1, refresh_calls);
    TEST_ASSERT_EQUAL_INT(0, update_call_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reserved_update_selectors);
    RUN_TEST(test_empty_update_avoids_network);
    RUN_TEST(test_refresh_is_required_before_planning);
    RUN_TEST(test_reference_uses_same_tool_default_otherwise_max);
    RUN_TEST(test_no_stable_and_ahead_are_skipped);
    RUN_TEST(test_equal_stable_reuses_installed_package);
    RUN_TEST(test_revision_is_normal_newer_version);
    RUN_TEST(test_sequential_failure_stops_later_families);
    RUN_TEST(test_context_and_catalog_failures);
    return UNITY_END();
}
