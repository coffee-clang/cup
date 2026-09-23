/*
 * Verifies install selector resolution, preference boundaries, complete group
 * prevalidation and explicit toolchain plans.
 */

#include "catalog_refresh.h"
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
static int context_begin_calls;
static CupError state_load_result;
static CupError package_catalog_load_result;
static CupError config_load_result;
static CupError package_result;
static CupError resolve_result;
static CupError artifact_result;
static int artifact_available;
static const char *unavailable_tool;
static char already_installed_entry[MAX_SELECTOR_LEN];
static int install_fail_call;
static CupError install_fail_result;
static CupError installed_valid_result;
static CupError catalog_refresh_result;
static int catalog_refresh_calls;
static CatalogRefreshDiagnostics catalog_refresh_diagnostics;
static int refresh_enables_artifact;

void setUp(void) {
    /* Curated policy fixtures cover component profiles and explicit toolchains. */
    memset(&standard_profile, 0, sizeof(standard_profile));
    strcpy(standard_profile.name, "standard");
    standard_profile.item_count = 4;
    strcpy(standard_profile.items[0], "compiler");
    strcpy(standard_profile.items[1], "linker");
    strcpy(standard_profile.items[2], "debugger");
    strcpy(standard_profile.items[3], "language-server");

    memset(&llvm_toolchain, 0, sizeof(llvm_toolchain));
    strcpy(llvm_toolchain.name, "llvm");
    llvm_toolchain.item_count = 6;
    strcpy(llvm_toolchain.items[0], "clang");
    strcpy(llvm_toolchain.items[1], "lldb");
    strcpy(llvm_toolchain.items[2], "lld");
    strcpy(llvm_toolchain.items[3], "clang-format");
    strcpy(llvm_toolchain.items[4], "clang-tidy");
    strcpy(llvm_toolchain.items[5], "clangd");

    memset(&gnu_toolchain, 0, sizeof(gnu_toolchain));
    strcpy(gnu_toolchain.name, "gnu");
    gnu_toolchain.item_count = 3;
    strcpy(gnu_toolchain.items[0], "gcc");
    strcpy(gnu_toolchain.items[1], "gdb");
    strcpy(gnu_toolchain.items[2], "ld");

    /* Boundary outcomes and observations reset independently of the policy fixtures. */
    install_calls = 0;
    resolver_calls = 0;
    config_load_calls = 0;
    preferences_load_calls = 0;
    preferences_load_result = CUP_OK;
    context_begin_result = CUP_OK;
    context_begin_calls = 0;
    state_load_result = CUP_OK;
    package_catalog_load_result = CUP_OK;
    config_load_result = CUP_OK;
    package_result = CUP_OK;
    resolve_result = CUP_OK;
    artifact_result = CUP_OK;
    artifact_available = 1;
    unavailable_tool = NULL;
    already_installed_entry[0] = '\0';
    install_fail_call = 0;
    install_fail_result = CUP_OK;
    installed_valid_result = CUP_OK;
    catalog_refresh_result = CUP_OK;
    catalog_refresh_calls = 0;
    catalog_refresh_diagnostics = CATALOG_REFRESH_REPORT_ERRORS;
    refresh_enables_artifact = 0;
    memset(installed_components, 0, sizeof(installed_components));
    memset(installed_entries, 0, sizeof(installed_entries));
    memset(installed_formats, 0, sizeof(installed_formats));
}

void tearDown(void) {
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)mode;
    context_begin_calls++;
    memset(context, 0, sizeof(*context));
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, target_override == NULL ? "linux-x64" : target_override);
    return context_begin_result;
}

CupError command_context_begin_initialize(CommandContext *context,
                                          const char *target_override,
                                          SystemLockMode mode) {
    return command_context_begin(context, target_override, mode);
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

CupError catalog_refresh_existing(int *updated, CatalogRefreshDiagnostics diagnostics) {
    catalog_refresh_diagnostics = diagnostics;
    TEST_ASSERT_NOT_NULL(updated);
    catalog_refresh_calls++;
    *updated = catalog_refresh_result == CUP_OK;
    if (catalog_refresh_result == CUP_OK && refresh_enables_artifact) {
        artifact_available = 1;
    }
    return catalog_refresh_result;
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

CupError tool_preferences_load(ToolPreferences *preferences, FILE *diagnostics) {
    (void)diagnostics;
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

const ToolPreference *tool_preferences_find(const ToolPreferences *preferences,
                                            const char *target,
                                            const char *component) {
    static ToolPreference preference;

    (void)preferences;
    (void)target;
    resolver_calls++;
    memset(&preference, 0, sizeof(preference));
    strcpy(preference.scope.host_platform, "linux-x64");
    strcpy(preference.scope.target_platform, target);
    strcpy(preference.scope.component, component);
    if (strcmp(component, "compiler") == 0) {
        strcpy(preference.tool, "gcc");
        return &preference;
    }
    if (strcmp(component, "debugger") == 0) {
        strcpy(preference.tool, "gdb");
        return &preference;
    }
    return NULL;
}

const InstallDefault *install_policy_find_default(const InstallPolicy *config,
                                                  const char *host,
                                                  const char *target,
                                                  const char *component) {
    static InstallDefault entry;
    const char *tool = NULL;

    (void)config;
    memset(&entry, 0, sizeof(entry));
    strcpy(entry.scope.host_platform, host);
    strcpy(entry.scope.target_platform, target);
    strcpy(entry.scope.component, component);
    if (strcmp(component, "compiler") == 0) tool = "clang";
    else if (strcmp(component, "linker") == 0) tool = "lld";
    else if (strcmp(component, "debugger") == 0) tool = "lldb";
    else if (strcmp(component, "language-server") == 0) tool = "clangd";
    if (tool == NULL) return NULL;
    strcpy(entry.tool, tool);
    return &entry;
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
    *available = strcmp(tool, "ld") != 0 &&
                 (unavailable_tool == NULL || strcmp(tool, unavailable_tool) != 0);
    return CUP_OK;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host,
                               const char *target,
                               const char *version) {
    if (identity == NULL || component == NULL || tool == NULL || host == NULL || target == NULL ||
        version == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    if (text_copy(identity->component, sizeof(identity->component), component) != CUP_OK ||
        text_copy(identity->tool, sizeof(identity->tool), tool) != CUP_OK ||
        text_copy(identity->host_platform, sizeof(identity->host_platform), host) != CUP_OK ||
        text_copy(identity->target_platform, sizeof(identity->target_platform), target) != CUP_OK ||
        text_copy(identity->version, sizeof(identity->version), version) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_selector_format_parts(
        buffer, size, identity->tool, identity->version);
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

CupError package_artifact_spec_build(PackageArtifactSpec *spec,
                                     const PackageCatalog *catalog,
                                     const PackageIdentity *identity,
                                     const char *format_name) {
    (void)catalog;
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(format_name);
    if (artifact_result != CUP_OK) {
        return artifact_result;
    }
    if (!artifact_available) {
        return CUP_ERR_NOT_AVAILABLE;
    }

    memset(spec, 0, sizeof(*spec));
    spec->identity = *identity;
    if (strcmp(format_name, "tar.gz") == 0) {
        spec->format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
    } else if (strcmp(format_name, "tar.xz") == 0) {
        spec->format = PACKAGE_ARCHIVE_FORMAT_TAR_XZ;
    } else if (strcmp(format_name, "zip") == 0) {
        spec->format = PACKAGE_ARCHIVE_FORMAT_ZIP;
    } else {
        return CUP_ERR_NOT_AVAILABLE;
    }
    strcpy(spec->package_url, "https://example.invalid/package");
    strcpy(spec->artifact_sha256,
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    return CUP_OK;
}

CupError package_install_artifact(const PackageArtifactSpec *spec) {
    char entry[MAX_SELECTOR_LEN];

    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_TRUE(install_calls < 8);
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_format_parts(entry,
                                      sizeof(entry),
                                      spec->identity.tool,
                                      spec->identity.version));
    strcpy(installed_components[install_calls], spec->identity.component);
    strcpy(installed_entries[install_calls], entry);
    strcpy(installed_formats[install_calls],
           spec->format == PACKAGE_ARCHIVE_FORMAT_TAR_GZ
               ? "tar.gz"
               : (spec->format == PACKAGE_ARCHIVE_FORMAT_TAR_XZ ? "tar.xz" : "zip"));
    install_calls++;
    if (install_fail_call == install_calls) {
        return install_fail_result;
    }
    return CUP_OK;
}

CupError installed_package_require_valid(const CupState *state, const PackageIdentity *identity) {
    (void)state;
    (void)identity;
    return installed_valid_result;
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

static void test_direct_selection(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          command_install("compiler", "clang@release-x", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(0, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_INT(0, catalog_refresh_calls);
    TEST_ASSERT_EQUAL_STRING("compiler", installed_components[0]);
    TEST_ASSERT_EQUAL_STRING("clang@release-x", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("tar.gz", installed_formats[0]);
}

static void test_tool_first_selection(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("clang@release-x", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(0, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("compiler", installed_components[0]);
    TEST_ASSERT_EQUAL_STRING("clang@release-x", installed_entries[0]);
}

static void test_tool_first_description_uses_selector_capacity(void) {
    const char *selector = "clang@1234567890123456789012345678901";

    TEST_ASSERT_TRUE(strlen(selector) >= MAX_IDENTIFIER_LEN);
    TEST_ASSERT_TRUE(strlen(selector) < MAX_SELECTOR_LEN);
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install(selector, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING(selector, installed_entries[0]);
}

static void test_tool_first_stable_selection(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("clang@stable", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(0, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("compiler", installed_components[0]);
    TEST_ASSERT_EQUAL_STRING("clang@1.0.0", installed_entries[0]);
}

static void test_abbreviated_install(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("compiler", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, resolver_calls);
    TEST_ASSERT_EQUAL_INT(2, config_load_calls);
    TEST_ASSERT_EQUAL_INT(2, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
}

static void test_profile_preferences(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(8, resolver_calls);
    TEST_ASSERT_EQUAL_INT(2, config_load_calls);
    TEST_ASSERT_EQUAL_INT(2, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(4, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("lld@1.0.0", installed_entries[1]);
    TEST_ASSERT_EQUAL_STRING("gdb@1.0.0", installed_entries[2]);
    TEST_ASSERT_EQUAL_STRING("clangd@1.0.0", installed_entries[3]);
}

static void test_explicit_toolchain(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("toolchain", "llvm", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(2, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(6, install_calls);
    TEST_ASSERT_EQUAL_STRING("clang@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("lldb@1.0.0", installed_entries[1]);
    TEST_ASSERT_EQUAL_STRING("lld@1.0.0", installed_entries[2]);
    TEST_ASSERT_EQUAL_STRING("clang-format@1.0.0", installed_entries[3]);
    TEST_ASSERT_EQUAL_STRING("clang-tidy@1.0.0", installed_entries[4]);
    TEST_ASSERT_EQUAL_STRING("clangd@1.0.0", installed_entries[5]);
}

static void test_toolchain_no_prefs(void) {
    preferences_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("toolchain", "llvm", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(6, install_calls);
}

static void test_group_prevalidation(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install("toolchain", "gnu", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, resolver_calls);
    TEST_ASSERT_EQUAL_INT(2, config_load_calls);
    TEST_ASSERT_EQUAL_INT(0, preferences_load_calls);
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_group_preflight_checks_later_members_before_mutation(void) {
    unavailable_tool = "gdb";

    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install("profile", "standard", NULL, NULL));
    TEST_ASSERT_TRUE(resolver_calls > 0);
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_direct_stable(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("compiler", "gcc@stable", "linux-arm64", "zip"));
    TEST_ASSERT_EQUAL_INT(1, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("zip", installed_formats[0]);
}

static void test_exact_installed_is_local_noop(void) {
    strcpy(already_installed_entry, "clang@release-x");
    package_catalog_load_result = CUP_ERR_CATALOG;
    catalog_refresh_result = CUP_ERR_FETCH;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_ALREADY_INSTALLED,
        command_install("compiler", "clang@release-x", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, install_calls);
    TEST_ASSERT_EQUAL_INT(0, catalog_refresh_calls);

    setUp();
    strcpy(already_installed_entry, "clang@release-x");
    installed_valid_result = CUP_ERR_INCONSISTENT_STATE;
    package_catalog_load_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INCONSISTENT_STATE,
        command_install("compiler", "clang@release-x", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, catalog_refresh_calls);
}

static void test_symbolic_refresh_failure_uses_valid_local_catalog(void) {
    catalog_refresh_result = CUP_ERR_FETCH;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("clang@stable", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, catalog_refresh_calls);
    TEST_ASSERT_EQUAL_INT(CATALOG_REFRESH_QUIET, catalog_refresh_diagnostics);
    TEST_ASSERT_EQUAL_INT(2, context_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
}

static void test_exact_miss_refreshes_once(void) {
    artifact_available = 0;
    refresh_enables_artifact = 1;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("clang@release-x", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, catalog_refresh_calls);
    TEST_ASSERT_EQUAL_INT(CATALOG_REFRESH_REPORT_ERRORS, catalog_refresh_diagnostics);
    TEST_ASSERT_EQUAL_INT(2, context_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, install_calls);
}

static void test_symbolic_refresh_failure_without_local_resolution_fails(void) {
    catalog_refresh_result = CUP_ERR_FETCH;
    artifact_available = 0;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, command_install("clang@stable", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, catalog_refresh_calls);
    TEST_ASSERT_EQUAL_INT(CATALOG_REFRESH_QUIET, catalog_refresh_diagnostics);
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_unknown_groups(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install("profile", "missing", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_install("toolchain", "missing", NULL, NULL));
}

static void test_plan_load_failures(void) {
    context_begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_install("compiler", "gcc@stable", NULL, NULL));

    context_begin_result = CUP_OK;
    state_load_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_install("compiler", "gcc@stable", NULL, NULL));

    state_load_result = CUP_OK;
    package_catalog_load_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install("compiler", "gcc@stable", NULL, NULL));

    package_catalog_load_result = CUP_OK;
    config_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_install("compiler", NULL, NULL, NULL));

    config_load_result = CUP_OK;
    preferences_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_install("compiler", NULL, NULL, NULL));
}

static void test_plan_failures(void) {
    resolve_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install("compiler", "gcc@stable", NULL, NULL));

    resolve_result = CUP_OK;
    package_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_install("compiler", "gcc@stable", NULL, NULL));

    package_result = CUP_OK;
    artifact_available = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_install("compiler", "gcc@stable", NULL, NULL));

    artifact_available = 1;
    artifact_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG,
                          command_install("compiler", "gcc@stable", NULL, "zip"));
}

static void test_invalid_existing(void) {
    strcpy(already_installed_entry, "gcc@1.0.0");
    installed_valid_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_install("compiler", "gcc@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, install_calls);
}

static void test_group_execution(void) {
    /* A package seen as installed by the shared plan may disappear before execution. Every
     * package must therefore be revalidated by package_install under its exclusive context. */
    strcpy(already_installed_entry, "gcc@1.0.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(4, install_calls);
    TEST_ASSERT_EQUAL_STRING("compiler", installed_components[0]);
    TEST_ASSERT_EQUAL_STRING("linker", installed_components[1]);

    /* The inverse race is also benign: a package installed after planning is skipped only after
     * package_install observes CUP_ERR_ALREADY_INSTALLED under the exclusive context. */
    setUp();
    install_fail_call = 1;
    install_fail_result = CUP_ERR_ALREADY_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_install("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(4, install_calls);

    setUp();
    install_fail_call = 2;
    install_fail_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_install("profile", "standard", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, install_calls);
    TEST_ASSERT_EQUAL_STRING("gcc@1.0.0", installed_entries[0]);
    TEST_ASSERT_EQUAL_STRING("lld@1.0.0", installed_entries[1]);

    setUp();
    install_fail_call = 1;
    install_fail_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          command_install("profile", "standard", NULL, NULL));
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_direct_selection);
    RUN_TEST(test_tool_first_selection);
    RUN_TEST(test_tool_first_description_uses_selector_capacity);
    RUN_TEST(test_tool_first_stable_selection);
    RUN_TEST(test_abbreviated_install);
    RUN_TEST(test_profile_preferences);
    RUN_TEST(test_explicit_toolchain);
    RUN_TEST(test_toolchain_no_prefs);
    RUN_TEST(test_group_prevalidation);
    RUN_TEST(test_group_preflight_checks_later_members_before_mutation);
    RUN_TEST(test_direct_stable);
    RUN_TEST(test_exact_installed_is_local_noop);
    RUN_TEST(test_symbolic_refresh_failure_uses_valid_local_catalog);
    RUN_TEST(test_exact_miss_refreshes_once);
    RUN_TEST(test_symbolic_refresh_failure_without_local_resolution_fails);
    RUN_TEST(test_unknown_groups);
    RUN_TEST(test_plan_load_failures);
    RUN_TEST(test_plan_failures);
    RUN_TEST(test_invalid_existing);
    RUN_TEST(test_group_execution);
    return UNITY_END();
}
