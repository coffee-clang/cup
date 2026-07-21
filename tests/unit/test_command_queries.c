/*
 * Test focus: Exercises state-query command decisions and output without duplicating the
 * complete CLI lifecycle integration.
 */

#include "command_context.h"
#include "installed_package.h"
#include "commands.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "package_metadata.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"
#include "system.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static CupState scenario_state;
static PackageCatalogEntry package_catalog_items[12];
static size_t package_catalog_count;
static PackageMetadataField package_metadata_fields[12];
static size_t package_metadata_count;
static CupError begin_result;
static CupError load_state_result;
static CupError load_catalog_result;
static CupError no_transaction_result;
static CupError valid_installed_result;
static CupError plan_build_result;
static CupError plan_apply_result;
static CupError plan_active_result;
static CupError plan_match_result;
static CupError state_save_result;
static CupError package_metadata_load_result;
static CupError path_join_result;
static CupError identity_result;
static CupError set_active_result;
static int try_catalog;
static int plan_matches;
static int end_calls;
static WrapperSpec plan_items[3];
static size_t plan_count;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    memset(&scenario_state, 0, sizeof(scenario_state));
    memset(package_catalog_items, 0, sizeof(package_catalog_items));
    memset(package_metadata_fields, 0, sizeof(package_metadata_fields));
    memset(plan_items, 0, sizeof(plan_items));
    package_catalog_count = 0;
    package_metadata_count = 0;
    begin_result = CUP_OK;
    load_state_result = CUP_OK;
    load_catalog_result = CUP_OK;
    no_transaction_result = CUP_OK;
    valid_installed_result = CUP_OK;
    plan_build_result = CUP_OK;
    plan_apply_result = CUP_OK;
    plan_active_result = CUP_OK;
    plan_match_result = CUP_OK;
    state_save_result = CUP_OK;
    package_metadata_load_result = CUP_OK;
    path_join_result = CUP_OK;
    identity_result = CUP_OK;
    set_active_result = CUP_OK;
    try_catalog = 0;
    plan_matches = 1;
    end_calls = 0;
    plan_count = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

static void fill_identity(PackageIdentity *item,
                          const char *component,
                          const char *host,
                          const char *target,
                          const char *entry) {
    const char *separator = strchr(entry, '@');
    size_t tool_length;

    memset(item, 0, sizeof(*item));
    strcpy(item->component, component);
    strcpy(item->host_platform, host);
    strcpy(item->target_platform, target);
    if (separator == NULL) {
        strcpy(item->tool, entry);
        return;
    }
    tool_length = (size_t)(separator - entry);
    memcpy(item->tool, entry, tool_length);
    item->tool[tool_length] = '\0';
    strcpy(item->version, separator + 1);
}

static void add_installed(const char *component,
                          const char *host,
                          const char *target,
                          const char *entry) {
    PackageIdentity *item = &scenario_state.installed[scenario_state.installed_count++];
    fill_identity(item, component, host, target, entry);
}

static void add_active(const char *component,
                       const char *host,
                       const char *target,
                       const char *entry) {
    PackageIdentity *item = &scenario_state.active[scenario_state.active_count++];
    fill_identity(item, component, host, target, entry);
}

static void add_catalog_entry(const char *component,
                              const char *tool,
                              const char *host,
                              const char *target,
                              const char *stable,
                              const char *versions) {
    PackageCatalogEntry *item = &package_catalog_items[package_catalog_count++];
    strcpy(item->component, component);
    strcpy(item->tool, tool);
    strcpy(item->host_platform, host);
    strcpy(item->target_platform, target);
    strcpy(item->stable_version, stable);
    strcpy(item->available_versions, versions);
}

static void add_info(const char *key, const char *value) {
    PackageMetadataField *field = &package_metadata_fields[package_metadata_count++];
    strcpy(field->key, key);
    strcpy(field->value, value);
}

static char *capture_result(CupError (*operation)(void), CupError *result) {
    FILE *capture = tmpfile();
    int saved;
    int capture_flush_result;
    int restore_result;
    int close_result;
    long length;
    char *output;

    TEST_ASSERT_NOT_NULL(capture);
    TEST_ASSERT_EQUAL_INT(0, fflush(stdout));
    saved = dup(STDOUT_FILENO);
    TEST_ASSERT_TRUE(saved >= 0);
    TEST_ASSERT_TRUE(dup2(fileno(capture), STDOUT_FILENO) >= 0);

    *result = operation();

    capture_flush_result = fflush(stdout);
    restore_result = dup2(saved, STDOUT_FILENO);
    close_result = close(saved);
    TEST_ASSERT_EQUAL_INT(0, capture_flush_result);
    TEST_ASSERT_TRUE(restore_result >= 0);
    TEST_ASSERT_EQUAL_INT(0, close_result);

    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_END));
    length = ftell(capture);
    TEST_ASSERT_TRUE(length >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    output = calloc((size_t)length + 1, 1);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_size_t((size_t)length, fread(output, 1, (size_t)length, capture));
    TEST_ASSERT_FALSE(ferror(capture));
    TEST_ASSERT_EQUAL_INT(0, fclose(capture));
    return output;
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError registry_validate_component(const char *component) {
    if (component == NULL || strcmp(component, "bad") == 0) {
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }
    return CUP_OK;
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)mode;
    TEST_ASSERT_NOT_NULL(context);
    if (begin_result != CUP_OK) {
        return begin_result;
    }
    memset(context, 0, sizeof(*context));
    context->runtime_available = 1;
    context->state = scenario_state;
    context->catalog.packages = package_catalog_items;
    context->catalog.count = package_catalog_count;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, target_override != NULL ? target_override : "linux-x64");
    return CUP_OK;
}

CupError command_context_begin_read_only(CommandContext *context, const char *target_override) {
    return command_context_begin(context, target_override, SYSTEM_LOCK_SHARED);
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    end_calls++;
}

CupError command_context_load_state(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_state_result;
}

CupError command_context_load_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    if (load_catalog_result == CUP_OK) {
        context->has_catalog = 1;
    }
    return load_catalog_result;
}

void command_context_try_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context->has_catalog = try_catalog;
}

CupError runtime_journal_require_none(void) {
    return no_transaction_result;
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *entry) {
    const char *separator = entry == NULL ? NULL : strchr(entry, '@');
    size_t tool_length;

    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    tool_length = (size_t)(separator - entry);
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    memcpy(identity->tool, entry, tool_length);
    identity->tool[tool_length] = '\0';
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, separator + 1);
    return CUP_OK;
}

CupError package_path_exists(const PackageIdentity *identity, int *exists) {
    if (strcmp(identity->tool, "inspecterr") == 0) {
        return CUP_ERR_FILESYSTEM;
    }
    *exists = strcmp(identity->tool, "missing") != 0;
    return CUP_OK;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    if (strcmp(identity->tool, "pathbad") == 0) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return buffer_write_result(snprintf(buffer, size, "/install/%s", identity->tool), size);
}

CupError package_validate(const char *base_path, const PackageIdentity *expected_identity) {
    (void)expected_identity;
    if (strstr(base_path, "/invalid") != NULL) {
        return CUP_ERR_VALIDATION;
    }
    if (strstr(base_path, "/io") != NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope) {
    size_t i;
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

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    int written;
    if (identity == NULL || identity->tool[0] == '\0' || identity->version[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    written = snprintf(buffer, size, "%s@%s", identity->tool, identity->version);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope) {
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(scope);
    memset(scope, 0, sizeof(*scope));
    strcpy(scope->component, identity->component);
    strcpy(scope->host_platform, identity->host_platform);
    strcpy(scope->target_platform, identity->target_platform);
    return CUP_OK;
}

int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

CupError package_catalog_is_stable(const PackageCatalog *catalog,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   int *is_stable) {
    size_t i;
    *is_stable = 0;
    for (i = 0; i < catalog->count; ++i) {
        const PackageCatalogEntry *item = &catalog->packages[i];
        if (strcmp(item->component, component) == 0 && strcmp(item->tool, tool) == 0 &&
            strcmp(item->host_platform, host_platform) == 0 &&
            strcmp(item->target_platform, target_platform) == 0 &&
            strcmp(item->stable_version, version) == 0) {
            *is_stable = 1;
            break;
        }
    }
    return CUP_OK;
}

void wrapper_plan_init(WrapperPlan *plan) {
    memset(plan, 0, sizeof(*plan));
}

void wrapper_plan_free(WrapperPlan *plan) {
    plan->items = NULL;
    plan->count = 0;
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    (void)state;
    plan->items = plan_items;
    plan->count = plan_count;
    return plan_build_result;
}

CupError wrapper_plan_apply(const WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    return plan_apply_result;
}

CupError wrapper_plan_build_active(WrapperPlan *plan, const PackageIdentity *default_entry) {
    TEST_ASSERT_NOT_NULL(default_entry);
    plan->items = plan_items;
    plan->count = plan_count;
    return plan_active_result;
}

CupError wrapper_plan_expected_matches(const WrapperPlan *plan, int *matches) {
    TEST_ASSERT_NOT_NULL(plan);
    *matches = plan_matches;
    return plan_match_result;
}

CupError package_request_parse(const char *component, const char *entry, PackageRequest *request) {
    const char *separator;
    size_t tool_length;
    (void)component;
    if (entry == NULL || strcmp(entry, "bad") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    separator = strchr(entry, '@');
    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(request, 0, sizeof(*request));
    tool_length = (size_t)(separator - entry);
    memcpy(request->selector.tool, entry, tool_length);
    request->selector.tool[tool_length] = '\0';
    strcpy(request->selector.release, separator + 1);
    strcpy(request->input_selector, entry);
    return CUP_OK;
}

int package_release_is_stable(const char *release) {
    return release != NULL && strcmp(release, "stable") == 0;
}

CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host_platform,
                                 const char *target_platform,
                                 PackageRequest *request) {
    (void)catalog;
    (void)component;
    (void)host_platform;
    (void)target_platform;
    if (strcmp(request->selector.tool, "unavailable") == 0) {
        return CUP_ERR_NOT_AVAILABLE;
    }
    strcpy(request->resolved_release,
           package_release_is_stable(request->selector.release) ? "2.0"
                                                                : request->selector.release);
    snprintf(request->resolved_selector,
             sizeof(request->resolved_selector),
             "%s@%s",
             request->selector.tool,
             request->resolved_release);
    return CUP_OK;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    if (identity_result != CUP_OK) {
        return identity_result;
    }
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->component, component);
    strcpy(identity->tool, tool);
    strcpy(identity->host_platform, host_platform);
    strcpy(identity->target_platform, target_platform);
    strcpy(identity->version, version);
    return CUP_OK;
}

CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package) {
    (void)state;
    (void)package;
    return valid_installed_result;
}

CupError state_set_active(CupState *state, const PackageIdentity *identity) {
    size_t i;
    if (set_active_result != CUP_OK) {
        return set_active_result;
    }
    for (i = 0; i < state->active_count; ++i) {
        PackageIdentity *item = &state->active[i];
        if (strcmp(item->component, identity->component) == 0 &&
            strcmp(item->host_platform, identity->host_platform) == 0 &&
            strcmp(item->target_platform, identity->target_platform) == 0) {
            *item = *identity;
            return CUP_OK;
        }
    }
    TEST_ASSERT_TRUE(state->active_count < MAX_ACTIVE_PACKAGES);
    state->active[state->active_count++] = *identity;
    return CUP_OK;
}

CupError state_save(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    return state_save_result;
}

void package_metadata_init(PackageMetadata *info) {
    memset(info, 0, sizeof(*info));
}

void package_metadata_free(PackageMetadata *info) {
    info->fields = NULL;
    info->count = 0;
}

CupError package_metadata_load(PackageMetadata *info, const char *path) {
    TEST_ASSERT_NOT_NULL(path);
    if (package_metadata_load_result == CUP_OK) {
        info->fields = package_metadata_fields;
        info->count = package_metadata_count;
    }
    return package_metadata_load_result;
}

const char *package_metadata_get(const PackageMetadata *info, const char *key) {
    size_t i;
    for (i = 0; i < info->count; ++i) {
        if (strcmp(info->fields[i].key, key) == 0) {
            return info->fields[i].value;
        }
    }
    return NULL;
}

const PackageMetadataField *package_metadata_next(const PackageMetadata *info,
                                                  const char *prefix,
                                                  size_t *cursor) {
    size_t prefix_length = strlen(prefix);
    while (*cursor < info->count) {
        const PackageMetadataField *field = &info->fields[(*cursor)++];
        if (strncmp(field->key, prefix, prefix_length) == 0) {
            return field;
        }
    }
    return NULL;
}

int package_metadata_next_command(const PackageMetadata *metadata,
                                  PackageCommand *command,
                                  size_t *cursor) {
    const PackageMetadataField *field = package_metadata_next(metadata, "entry.", cursor);

    if (field == NULL) {
        return 0;
    }
    snprintf(command->name, sizeof(command->name), "%s", field->key + 6);
    snprintf(command->path, sizeof(command->path), "%s", field->value);
    return 1;
}

CupError path_join(char *buffer, size_t size, const char *left, const char *right) {
    if (path_join_result != CUP_OK) {
        return path_join_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s/%s", left, right), size);
}

void package_request_print(FILE *stream, const PackageRequest *request) {
    fputs(request->resolved_selector[0] != '\0' ? request->resolved_selector
                                                : request->input_selector,
          stream);
}

static CupError run_list_empty(void) {
    return command_list(NULL, NULL);
}

static CupError run_list_full(void) {
    return command_list(NULL, NULL);
}

static CupError run_current(void) {
    return command_info(NULL, NULL);
}

static CupError run_search_all(void) {
    return command_search(NULL, NULL);
}

static CupError run_search_target(void) {
    return command_search("compiler", "macos-x64");
}

static CupError run_inspect(void) {
    return command_inspect("compiler", "clang@stable", NULL);
}

static CupError run_info_empty(void) {
    return command_info("compiler", "windows-x64");
}

static CupError run_search_component(void) {
    return command_search("compiler", NULL);
}

static CupError run_search_all_target(void) {
    return command_search(NULL, "windows-x64");
}

static CupError run_inspect_plain(void) {
    return command_inspect("compiler", "clang@1.0", NULL);
}

static CupError run_list_component(void) {
    return command_list("compiler", NULL);
}

static CupError run_list_target(void) {
    return command_list(NULL, "windows-x64");
}

static CupError run_list_both(void) {
    return command_list("compiler", "windows-x64");
}

static CupError run_info_component(void) {
    return command_info("compiler", NULL);
}

static CupError run_info_target(void) {
    return command_info(NULL, "windows-x64");
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_list_empty(void) {
    CupError result;
    char *output = capture_result(run_list_empty, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No packages installed"));
    free(output);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT, command_list("bad", NULL));
    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_list(NULL, NULL));
}

static void test_list_entries(void) {
    CupError result;
    char *output;

    add_installed("compiler", "macos-x64", "linux-x64", "clang@2.0");
    add_installed("compiler", "linux-x64", "linux-x64", "broken");
    add_installed("compiler", "linux-x64", "linux-x64", "inspecterr@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "missing@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "pathbad@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "invalid@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "io@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_installed("compiler", "linux-x64", "windows-x64", "gcc@1.0");
    add_active("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "1.0,2.0");
    try_catalog = 1;

    output = capture_result(run_list_full, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "invalid state record"));
    TEST_ASSERT_NOT_NULL(strstr(output, "could not inspect package path"));
    TEST_ASSERT_NOT_NULL(strstr(output, "missing on disk"));
    TEST_ASSERT_NOT_NULL(strstr(output, "could not construct package path"));
    TEST_ASSERT_NOT_NULL(strstr(output, "invalid on disk"));
    TEST_ASSERT_NOT_NULL(strstr(output, "could not inspect package"));
    TEST_ASSERT_NOT_NULL(strstr(output, "default, stable"));
    TEST_ASSERT_NOT_NULL(strstr(output, "target windows-x64"));
    free(output);
}

static void test_default_flow(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_default(NULL, "clang@1.0", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_default("compiler", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_default("compiler", "bad", NULL));

    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0");
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_default("compiler", "clang@stable", NULL));

    reset_scenario();
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0");
    plan_apply_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    no_transaction_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    state_save_result = CUP_ERR_STATE_SAVE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_SAVE, command_default("compiler", "clang@1.0", NULL));
}

static void test_current_states(void) {
    CupError result;
    char *output;

    plan_count = 2;
    strcpy(plan_items[0].name, "clang");
    strcpy(plan_items[1].name, "clang++");
    add_active("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "2.0");
    try_catalog = 1;
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "clang@2.0 (stable)"));
    TEST_ASSERT_NOT_NULL(strstr(output, "clang, clang++"));
    TEST_ASSERT_NOT_NULL(strstr(output, "status: active"));
    free(output);

    reset_scenario();
    add_active("compiler", "linux-x64", "linux-x64", "broken");
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "status: invalid"));
    free(output);

    reset_scenario();
    plan_matches = 0;
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    free(output);
}

static void test_search_catalog(void) {
    CupError result;
    char *output;

    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "1.0,2.0");
    add_catalog_entry("compiler", "clang", "linux-x64", "windows-x64", "2.0", "2.0");
    add_catalog_entry("compiler", "gcc", "linux-x64", "linux-x64", "15.1", "15.1");
    add_catalog_entry("debugger", "gdb", "linux-x64", "linux-x64", "16.0", "16.0");

    output = capture_result(run_search_all, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "compiler:"));
    TEST_ASSERT_NOT_NULL(strstr(output, "debugger:"));
    TEST_ASSERT_NOT_NULL(strstr(output, "windows-x64: stable 2.0"));
    free(output);

    output = capture_result(run_search_target, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No tools are available"));
    free(output);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT, command_search("bad", NULL));
}

static void test_inspect_output(void) {
    CupError result;
    char *output;

    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_info("package.component", "compiler");
    add_info("package.tool", "clang");
    add_info("package.version", "2.0");
    add_info("platform.host", "linux-x64");
    add_info("platform.target", "linux-x64");
    add_info("entry.clang", "bin/clang");
    add_info("features.cxx", "enabled");
    add_info("contents.runtime", "included");
    add_info("config.flags", "--static");

    output = capture_result(run_inspect, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "Package information"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Commands:"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Features:"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Contents:"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Build/config:"));
    free(output);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_inspect(NULL, "clang@1.0", NULL));
    package_metadata_load_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_inspect("compiler", "clang@1.0", NULL));
}

static void test_state_failures(void) {
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_list(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_info(NULL, NULL));

    reset_scenario();
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_default("compiler", "clang@stable", NULL));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_default("compiler", "unavailable@1.0", NULL));

    reset_scenario();
    valid_installed_result = CUP_ERR_NOT_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    plan_build_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_default("compiler", "clang@1.0", NULL));
}

static void test_metadata_variants(void) {
    CupError result;
    char *output;

    output = capture_result(run_info_empty, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No current default"));
    free(output);

    reset_scenario();
    plan_count = 0;
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "commands: (none)"));
    free(output);

    reset_scenario();
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    plan_active_result = CUP_ERR_FILESYSTEM;
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    free(output);

    reset_scenario();
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    plan_match_result = CUP_ERR_FILESYSTEM;
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    free(output);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT, command_info("bad", NULL));
}

static void test_search_variants(void) {
    CupError result;
    char *output;

    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "2.0");
    add_catalog_entry("compiler", "clang", "macos-x64", "linux-x64", "2.0", "2.0");
    add_catalog_entry("debugger", "gdb", "linux-x64", "linux-x64", "16.0", "16.0");
    output = capture_result(run_search_component, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "Available tools"));
    TEST_ASSERT_NOT_NULL(strstr(output, "clang"));
    free(output);

    output = capture_result(run_search_all_target, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No packages are available"));
    free(output);

    reset_scenario();
    add_catalog_entry("compiler", "clang", "linux-x64", "windows-x64", "2.0", "2.0");
    output = capture_result(run_search_all_target, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "windows-x64: stable 2.0"));
    free(output);

    reset_scenario();
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_search(NULL, NULL));
}

static void test_inspect_failures(void) {
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_inspect("compiler", "clang@1.0", NULL));

    reset_scenario();
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_inspect("compiler", "clang@stable", NULL));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          command_inspect("compiler", "unavailable@1.0", NULL));

    reset_scenario();
    valid_installed_result = CUP_ERR_NOT_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_inspect("compiler", "clang@1.0", NULL));

    reset_scenario();
    path_join_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_inspect("compiler", "clang@1.0", NULL));

    reset_scenario();
    add_info("package.component", "compiler");
    add_info("package.tool", "clang");
    add_info("package.version", "1.0");
    {
        CupError result;
        char *output = capture_result(run_inspect_plain, &result);
        TEST_ASSERT_EQUAL_INT(CUP_OK, result);
        TEST_ASSERT_NULL(strstr(output, "host             "));
        free(output);
    }
}

static void test_list_variants(void) {
    CupError result;
    char *output;

    output = capture_result(run_list_component, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No compiler packages"));
    free(output);

    output = capture_result(run_list_target, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "target 'windows-x64'"));
    free(output);

    output = capture_result(run_list_both, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No compiler packages"));
    free(output);

    reset_scenario();
    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "2.0");
    try_catalog = 1;
    output = capture_result(run_list_full, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "clang@2.0 [target linux-x64] (stable)"));
    TEST_ASSERT_NULL(strstr(output, "default"));
    free(output);

    reset_scenario();
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0");
    add_installed("compiler", "linux-x64", "linux-x64", "clang@2.0");
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    add_catalog_entry("compiler", "clang", "linux-x64", "linux-x64", "2.0", "1.0,2.0");
    try_catalog = 1;
    output = capture_result(run_list_full, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "clang@1.0 [target linux-x64] (default)"));
    TEST_ASSERT_NOT_NULL(strstr(output, "clang@2.0 [target linux-x64] (stable)"));
    TEST_ASSERT_NULL(strstr(output, "default, stable"));
    free(output);

    reset_scenario();
    add_installed("compiler", "linux-x64", "linux-x64", "clang@1.0");
    output = capture_result(run_list_full, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NULL(strstr(output, "stable"));
    free(output);
}

static void test_default_errors(void) {
    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    identity_result = CUP_ERR_INVALID_RELEASE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, command_default("compiler", "clang@1.0", NULL));

    reset_scenario();
    set_active_result = CUP_ERR_ACTIVE_FULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ACTIVE_FULL, command_default("compiler", "clang@1.0", NULL));
}

static void test_metadata_filters(void) {
    CupError result;
    char *output;

    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_info(NULL, NULL));

    reset_scenario();
    add_active("compiler", "macos-x64", "linux-x64", "clang@1.0");
    add_active("debugger", "linux-x64", "linux-x64", "gdb@1.0");
    output = capture_result(run_info_component, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No current defaults for component"));
    free(output);

    reset_scenario();
    add_active("compiler", "linux-x64", "windows-x64", "clang@1.0");
    output = capture_result(run_info_empty, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "Current default for component"));
    free(output);

    reset_scenario();
    output = capture_result(run_info_component, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No current defaults for component"));
    free(output);

    output = capture_result(run_info_target, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No current defaults for host"));
    free(output);

    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_OK, result);
    TEST_ASSERT_NOT_NULL(strstr(output, "No current defaults for host"));
    free(output);

    reset_scenario();
    valid_installed_result = CUP_ERR_NOT_INSTALLED;
    add_active("compiler", "linux-x64", "linux-x64", "clang@1.0");
    output = capture_result(run_current, &result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, result);
    free(output);
}

static void test_inspect_setup(void) {
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_inspect("compiler", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_inspect("compiler", "bad", NULL));

    begin_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_inspect("compiler", "clang@1.0", NULL));

    reset_scenario();
    identity_result = CUP_ERR_INVALID_RELEASE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE, command_inspect("compiler", "clang@1.0", NULL));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_inspect("compiler", "pathbad@1.0", NULL));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_list_empty);
    RUN_TEST(test_list_entries);
    RUN_TEST(test_default_flow);
    RUN_TEST(test_current_states);
    RUN_TEST(test_search_catalog);
    RUN_TEST(test_inspect_output);
    RUN_TEST(test_state_failures);
    RUN_TEST(test_metadata_variants);
    RUN_TEST(test_search_variants);
    RUN_TEST(test_inspect_failures);
    RUN_TEST(test_list_variants);
    RUN_TEST(test_default_errors);
    RUN_TEST(test_metadata_filters);
    RUN_TEST(test_inspect_setup);
    return UNITY_END();
}
