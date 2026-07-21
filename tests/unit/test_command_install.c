/*
 * Test focus: Verifies install selector resolution, preference boundaries, complete group
 * prevalidation and explicit toolchain plans.
 */

#include "command_context.h"
#include "commands.h"
#include "package_selector.h"
#include "package_request.h"
#include "package_install.h"
#include "install_policy.h"
#include "tool_preferences.h"
#include "package_catalog.h"
#include "registry.h"
#include "state.h"
#include "text.h"
#include "unity.h"

#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static InstallNamedList standard_profile;
static InstallNamedList llvm_toolchain;
static InstallNamedList gnu_toolchain;
static int install_calls;
static char installed_components[8][MAX_IDENTIFIER_LEN];
static char installed_entries[8][MAX_SELECTOR_LEN];
static char installed_formats[8][MAX_IDENTIFIER_LEN];
static int resolver_calls;
static int config_load_calls;
static int preferences_load_calls;
static CupError preferences_load_result;
static CupError context_begin_result;
static CupError state_load_result;
static CupError package_catalog_load_result;
static CupError config_load_result;
static CupError resolver_result;
static CupError package_result;
static CupError resolve_result;
static CupError version_result;
static CupError format_result;
static CupError default_format_result;
static int version_available;
static int format_available;
static char already_installed_entry[MAX_SELECTOR_LEN];
static int install_fail_call;
static CupError install_fail_result;
static CupError installed_valid_result;

/* Fixture lifecycle and local construction helpers. */

void setUp(void) {
    /* Curated policy fixtures cover component profiles and explicit toolchains. */
    memset(&standard_profile, 0, sizeof(standard_profile));
    strcpy(standard_profile.name, "standard");
    standard_profile.item_count = 2;
    strcpy(standard_profile.items[0], "compiler");
    strcpy(standard_profile.items[1], "linker");

    memset(&llvm_toolchain, 0, sizeof(llvm_toolchain));
    strcpy(llvm_toolchain.name, "llvm");
    llvm_toolchain.item_count = 3;
    strcpy(llvm_toolchain.items[0], "clang");
    strcpy(llvm_toolchain.items[1], "lldb");
    strcpy(llvm_toolchain.items[2], "lld");

    memset(&gnu_toolchain, 0, sizeof(gnu_toolchain));
    strcpy(gnu_toolchain.name, "gnu");
    gnu_toolchain.item_count = 4;
    strcpy(gnu_toolchain.items[0], "gcc");
    strcpy(gnu_toolchain.items[1], "gdb");
    strcpy(gnu_toolchain.items[2], "ld");
    strcpy(gnu_toolchain.items[3], "valgrind");

    /* Boundary outcomes and observations reset independently of the policy fixtures. */
    install_calls = 0;
    resolver_calls = 0;
    config_load_calls = 0;
    preferences_load_calls = 0;
    preferences_load_result = CUP_OK;
    context_begin_result = CUP_OK;
    state_load_result = CUP_OK;
    package_catalog_load_result = CUP_OK;
    config_load_result = CUP_OK;
    resolver_result = CUP_OK;
    package_result = CUP_OK;
    resolve_result = CUP_OK;
    version_result = CUP_OK;
    format_result = CUP_OK;
    default_format_result = CUP_OK;
    version_available = 1;
    format_available = 1;
    already_installed_entry[0] = '\0';
    install_fail_call = 0;
    install_fail_result = CUP_OK;
    installed_valid_result = CUP_OK;
    memset(installed_components, 0, sizeof(installed_components));
    memset(installed_entries, 0, sizeof(installed_entries));
    memset(installed_formats, 0, sizeof(installed_formats));
}

void tearDown(void) {
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)mode;
    memset(context, 0, sizeof(*context));
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, target_override == NULL ? "linux-x64" : target_override);
    return context_begin_result;
}

void command_context_end(CommandContext *context) {
    (void)context;
}

CupError command_context_load_state(CommandContext *context) {
    (void)context;
    return state_load_result;
}

CupError command_context_load_catalog(CommandContext *context) {
    context->has_catalog = 1;
    return package_catalog_load_result;
}

void install_policy_init(InstallPolicy *config) {
    memset(config, 0, sizeof(*config));
}

CupError install_policy_load(InstallPolicy *config) {
    (void)config;
    config_load_calls++;
    return config_load_result;
}

void tool_preferences_init(ToolPreferences *preferences) {
    memset(preferences, 0, sizeof(*preferences));
}

CupError tool_preferences_load(const InstallPolicy *config, ToolPreferences *preferences) {
    (void)config;
    (void)preferences;
    preferences_load_calls++;
    return preferences_load_result;
}

const InstallNamedList *install_policy_find_profile(const InstallPolicy *config, const char *name) {
    (void)config;
    return strcmp(name, "standard") == 0 ? &standard_profile : NULL;
}

const InstallNamedList *install_policy_find_toolchain(const InstallPolicy *config,
                                                      const char *name) {
    (void)config;
    if (strcmp(name, "llvm") == 0) {
        return &llvm_toolchain;
    }
    return strcmp(name, "gnu") == 0 ? &gnu_toolchain : NULL;
}

CupError tool_preferences_resolve(const InstallPolicy *config,
                                  const ToolPreferences *preferences,
                                  const char *host,
                                  const char *target,
                                  const char *component,
                                  char *tool,
                                  size_t tool_size,
                                  ToolPreferenceSource *source) {
    const char *selected = NULL;

    (void)config;
    (void)preferences;
    (void)host;
    (void)target;
    resolver_calls++;
    if (resolver_result != CUP_OK) {
        return resolver_result;
    }
    if (strcmp(component, "compiler") == 0) {
        selected = "gcc";
    } else if (strcmp(component, "linker") == 0) {
        selected = "lld";
    } else if (strcmp(component, "debugger") == 0) {
        selected = "gdb";
    } else if (strcmp(component, "language-server") == 0) {
        selected = "clangd";
    }
    if (selected == NULL) {
        return CUP_ERR_VALIDATION;
    }
    *source = TOOL_PREFERENCE_USER;
    return text_copy(tool, tool_size, selected);
}

CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package) {
    (void)state;
    (void)package;
    return installed_valid_result;
}

CupError package_request_parse(const char *component, const char *entry, PackageRequest *request) {
    CupError err;

    memset(request, 0, sizeof(*request));
    err = package_selector_parse_parts(entry,
                                       request->selector.tool,
                                       sizeof(request->selector.tool),
                                       request->selector.release,
                                       sizeof(request->selector.release));
    if (err != CUP_OK) {
        return err;
    }
    err = registry_validate_tool(component, request->selector.tool);
    if (err != CUP_OK) {
        return err;
    }
    return text_copy(request->input_selector, sizeof(request->input_selector), entry);
}

CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host,
                                 const char *target,
                                 PackageRequest *request) {
    (void)catalog;
    (void)component;
    (void)host;
    (void)target;
    if (resolve_result != CUP_OK) {
        return resolve_result;
    }
    if (strcmp(request->selector.release, "stable") == 0) {
        strcpy(request->resolved_release, "1.0.0");
    } else {
        strcpy(request->resolved_release, request->selector.release);
    }
    return package_selector_format_parts(request->resolved_selector,
                                         sizeof(request->resolved_selector),
                                         request->selector.tool,
                                         request->resolved_release);
}

CupError package_catalog_has_package(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host,
                                     const char *target,
                                     int *available) {
    (void)catalog;
    (void)component;
    (void)host;
    (void)target;
    if (package_result != CUP_OK) {
        return package_result;
    }
    *available = strcmp(tool, "ld") != 0;
    return CUP_OK;
}

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host,
                                     const char *target,
                                     const char *version,
                                     int *available) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host;
    (void)target;
    (void)version;
    *available = version_available;
    return version_result;
}

CupError package_catalog_has_format(const PackageCatalog *catalog,
                                    const char *component,
                                    const char *tool,
                                    const char *host,
                                    const char *target,
                                    const char *format,
                                    int *available) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host;
    (void)target;
    (void)format;
    *available = format_available;
    return format_result;
}

CupError package_catalog_get_default_format(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host,
                                            const char *target) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host;
    (void)target;
    if (default_format_result != CUP_OK) {
        return default_format_result;
    }
    return text_copy(buffer, size, "tar.xz");
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host,
                                        const char *target,
                                        const char *entry) {
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];
    CupError err =
        package_selector_parse_parts(entry, tool, sizeof(tool), version, sizeof(version));

    if (err != CUP_OK) {
        return err;
    }
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    strcpy(identity->tool, tool);
    strcpy(identity->host_platform, host);
    strcpy(identity->target_platform, target);
    strcpy(identity->version, version);
    return CUP_OK;
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    char entry[MAX_SELECTOR_LEN];

    (void)state;
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_format_parts(entry, sizeof(entry), identity->tool, identity->version));
    return already_installed_entry[0] != '\0' && strcmp(entry, already_installed_entry) == 0 ? 0
                                                                                             : -1;
}

CupError package_install(const char *component,
                         const char *entry,
                         const char *target_override,
                         const char *format_override) {
    (void)target_override;
    TEST_ASSERT_NOT_NULL(format_override);
    TEST_ASSERT_TRUE(install_calls < 8);
    strcpy(installed_components[install_calls], component);
    strcpy(installed_entries[install_calls], entry);
    strcpy(installed_formats[install_calls], format_override);
    install_calls++;
    if (install_fail_call == install_calls) {
        return install_fail_result;
    }
    return CUP_OK;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_direct_selection(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          command_install_request("COMPILER", "Clang@Release-X", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(0, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("compiler", installed_components[0]);
    TEST_ASSERT_EQUAL_STRING("clang@Release-X", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("tar.xz", installed_formats[0]);
}

static void test_abbreviated_install(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("compiler", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, resolver_calls);
    TEST_ASSERT_EQUAL_INT(1, config_load_calls);
    TEST_ASSERT_EQUAL_INT(1, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
}

static void test_profile_preferences(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("PROFILE", "STANDARD", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, resolver_calls);
    TEST_ASSERT_EQUAL_INT(1, config_load_calls);
    TEST_ASSERT_EQUAL_INT(1, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(2, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("lld@1.0.0", installed_entries[1]);
}

static void test_explicit_toolchain(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("TOOLCHAIN", "LLVM", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(1, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(3, install_calls);
    TEST_ASSERT_EQUAL_STRING("clang@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("lldb@1.0.0", installed_entries[1]);
    TEST_ASSERT_EQUAL_STRING("lld@1.0.0", installed_entries[2]);
}

static void test_toolchain_no_prefs(void) {
    preferences_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("toolchain", "llvm", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(3, install_calls);
}

static void test_group_prevalidation(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install_request("toolchain", "gnu", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(1, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_direct_stable(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("compiler", "GCC", "linux-arm64", "zip"));
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("zip", installed_formats[0]);
}

static void test_invalid_groups(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install_request("profile", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install_request("profile", "missing", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install_request("toolchain", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install_request("toolchain", "missing", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT,
                          command_install_request("unknown", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_install_request(NULL, NULL, NULL, NULL));
}

static void test_plan_load_failures(void) {
    context_begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_install_request("compiler", "gcc", NULL, NULL));

    context_begin_result = CUP_OK;
    state_load_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_install_request("compiler", "gcc", NULL, NULL));

    state_load_result = CUP_OK;
    package_catalog_load_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install_request("compiler", "gcc", NULL, NULL));

    package_catalog_load_result = CUP_OK;
    config_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_install_request("compiler", NULL, NULL, NULL));

    config_load_result = CUP_OK;
    preferences_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_install_request("compiler", NULL, NULL, NULL));
}

static void test_plan_failures(void) {
    resolver_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_install_request("compiler", NULL, NULL, NULL));

    resolver_result = CUP_OK;
    resolve_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install_request("compiler", "gcc", NULL, NULL));

    resolve_result = CUP_OK;
    package_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install_request("compiler", "gcc", NULL, NULL));

    package_result = CUP_OK;
    version_available = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install_request("compiler", "gcc", NULL, NULL));

    version_available = 1;
    format_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install_request("compiler", "gcc", NULL, "zip"));

    format_result = CUP_OK;
    format_available = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install_request("compiler", "gcc", NULL, "zip"));

    format_available = 1;
    default_format_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install_request("compiler", "gcc", NULL, NULL));
}

static void test_invalid_existing(void) {
    strcpy(already_installed_entry, "gcc@1.0.0");
    installed_valid_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_install_request("compiler", "gcc", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_group_execution(void) {
    strcpy(already_installed_entry, "gcc@1.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("linker", installed_components[0]);

    setUp();
    install_fail_call = 1;
    install_fail_result = CUP_ERR_ALREADY_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install_request("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, install_calls);

    setUp();
    install_fail_call = 2;
    install_fail_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_install_request("profile", "standard", NULL, NULL));

    setUp();
    install_fail_call = 1;
    install_fail_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          command_install_request("profile", "standard", NULL, NULL));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_direct_selection);
    RUN_TEST(test_abbreviated_install);
    RUN_TEST(test_profile_preferences);
    RUN_TEST(test_explicit_toolchain);
    RUN_TEST(test_toolchain_no_prefs);
    RUN_TEST(test_group_prevalidation);
    RUN_TEST(test_direct_stable);
    RUN_TEST(test_invalid_groups);
    RUN_TEST(test_plan_load_failures);
    RUN_TEST(test_plan_failures);
    RUN_TEST(test_invalid_existing);
    RUN_TEST(test_group_execution);
    return UNITY_END();
}
