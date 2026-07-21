/*
 * Test focus: Exercises install preparation, cache refresh, commit boundaries, default updates
 * and rollback decisions.
 */

#include "command_context.h"
#include "package_cache.h"
#include "package_install.h"
#include "installed_package.h"
#include "package_selector.h"
#include "package_request.h"
#include "package_extract.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "wrappers.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#define MAX_STEPS 4

static CupState initial_state;
static CupError parse_result;
static CupError context_result;
static CupError guard_result;
static CupError load_state_result;
static CupError load_catalog_result;
static CupError resolve_result;
static CupError identity_result;
static CupError version_result;
static int version_available;
static CupError absent_result;
static CupError valid_installed_result;
static CupError default_format_result;
static CupError format_result;
static int format_supported;
static CupError tmp_result;
static CupError install_path_result;
static CupError begin_result;
static CupError url_result;
static CupError checksum_url_result;
static CupError fetch_results[MAX_STEPS];
static PackageCacheSource fetch_sources[MAX_STEPS];
static CupError extract_results[MAX_STEPS];
static CupError validate_results[MAX_STEPS];
static int interrupt_values[MAX_STEPS];
static CupError discard_result;
static CupError remove_results[MAX_STEPS];
static CupError ensure_dir_result;
static CupError read_only_result;
static CupError parent_result;
static CupError move_results[MAX_STEPS];
static SystemCommitState move_states[MAX_STEPS];
static CupError add_state_result;
static const char *current_active;
static CupError set_active_result;
static CupError plan_build_result;
static CupError save_result;
static CupError clear_results[MAX_STEPS];
static CupError plan_apply_result;
static CupError entry_build_result;
static CupError entry_parse_result;

static int context_end_calls;
static int plan_init_calls;
static int plan_free_calls;
static int plan_build_calls;
static int plan_apply_calls;
static int fetch_calls;
static int extract_calls;
static int validate_calls;
static int interrupt_calls;
static int discard_calls;
static int remove_calls;
static int ensure_dir_calls;
static int move_calls;
static int clear_calls;
static int save_calls;

static void reset_scenario(void) {
    size_t i;

    memset(&initial_state, 0, sizeof(initial_state));
    parse_result = CUP_OK;
    context_result = CUP_OK;
    guard_result = CUP_OK;
    load_state_result = CUP_OK;
    load_catalog_result = CUP_OK;
    resolve_result = CUP_OK;
    identity_result = CUP_OK;
    version_result = CUP_OK;
    version_available = 1;
    absent_result = CUP_OK;
    valid_installed_result = CUP_OK;
    default_format_result = CUP_OK;
    format_result = CUP_OK;
    format_supported = 1;
    tmp_result = CUP_OK;
    install_path_result = CUP_OK;
    begin_result = CUP_OK;
    url_result = CUP_OK;
    checksum_url_result = CUP_OK;
    discard_result = CUP_OK;
    ensure_dir_result = CUP_OK;
    read_only_result = CUP_OK;
    parent_result = CUP_OK;
    add_state_result = CUP_OK;
    current_active = NULL;
    set_active_result = CUP_OK;
    plan_build_result = CUP_OK;
    save_result = CUP_OK;
    plan_apply_result = CUP_OK;
    entry_build_result = CUP_OK;
    entry_parse_result = CUP_OK;

    context_end_calls = 0;
    plan_init_calls = 0;
    plan_free_calls = 0;
    plan_build_calls = 0;
    plan_apply_calls = 0;
    fetch_calls = 0;
    extract_calls = 0;
    validate_calls = 0;
    interrupt_calls = 0;
    discard_calls = 0;
    remove_calls = 0;
    ensure_dir_calls = 0;
    move_calls = 0;
    clear_calls = 0;
    save_calls = 0;

    for (i = 0; i < MAX_STEPS; ++i) {
        fetch_results[i] = CUP_OK;
        fetch_sources[i] = PACKAGE_CACHE_SOURCE_NETWORK;
        extract_results[i] = CUP_OK;
        validate_results[i] = CUP_OK;
        interrupt_values[i] = 0;
        remove_results[i] = CUP_OK;
        move_results[i] = CUP_OK;
        move_states[i] = SYSTEM_COMMIT_DURABLE;
        clear_results[i] = CUP_OK;
    }
}

static void add_entry(const char *component,
                      const char *host,
                      const char *target,
                      const char *entry) {
    PackageIdentity *item = &initial_state.installed[initial_state.installed_count++];
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];

    const char *separator = strchr(entry, '@');

    memset(item, 0, sizeof(*item));
    strcpy(item->component, component);
    strcpy(item->host_platform, host);
    strcpy(item->target_platform, target);
    if (separator != NULL) {
        size_t tool_length = (size_t)(separator - entry);
        memcpy(tool, entry, tool_length);
        tool[tool_length] = '\0';
        strcpy(version, separator + 1);
        strcpy(item->tool, tool);
        strcpy(item->version, version);
    }
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

CupError package_request_parse(const char *component, const char *entry, PackageRequest *request) {
    TEST_ASSERT_NOT_NULL(request);
    if (parse_result != CUP_OK) {
        return parse_result;
    }
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(entry);
    memset(request, 0, sizeof(*request));
    strcpy(request->selector.tool, "clang");
    strcpy(request->selector.release, strstr(entry, "stable") != NULL ? "stable" : "22.1.5");
    strcpy(request->input_selector, entry);
    return CUP_OK;
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    memset(context, 0, sizeof(*context));
    if (context_result != CUP_OK) {
        return context_result;
    }
    context->state = initial_state;
    strcpy(context->host_platform, "linux-x64");
    strcpy(context->target_platform, target_override == NULL ? "linux-x64" : target_override);
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context_end_calls++;
}

CupError runtime_journal_require_none(void) {
    return guard_result;
}

CupError command_context_load_state(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_state_result;
}

CupError command_context_load_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    return load_catalog_result;
}

CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host_platform,
                                 const char *target_platform,
                                 PackageRequest *request) {
    (void)catalog;
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(host_platform);
    TEST_ASSERT_NOT_NULL(target_platform);
    TEST_ASSERT_NOT_NULL(request);
    if (resolve_result != CUP_OK) {
        return resolve_result;
    }
    strcpy(request->resolved_release, "22.1.5");
    strcpy(request->resolved_selector, "clang@22.1.5");
    return CUP_OK;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    TEST_ASSERT_NOT_NULL(identity);
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

CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available) {
    (void)catalog;
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_NOT_NULL(host_platform);
    TEST_ASSERT_NOT_NULL(target_platform);
    TEST_ASSERT_NOT_NULL(version);
    TEST_ASSERT_NOT_NULL(is_available);
    *is_available = version_available;
    return version_result;
}

CupError installed_package_require_absent(const CupState *state, const PackageIdentity *package) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(package);
    return absent_result;
}

CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(package);
    return valid_installed_result;
}

CupError package_catalog_get_default_format(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    if (default_format_result != CUP_OK) {
        return default_format_result;
    }
    return snprintf(buffer, size, "tar.gz") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_catalog_has_format(const PackageCatalog *catalog,
                                    const char *component,
                                    const char *tool,
                                    const char *host_platform,
                                    const char *target_platform,
                                    const char *format,
                                    int *is_supported) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    TEST_ASSERT_NOT_NULL(format);
    TEST_ASSERT_NOT_NULL(is_supported);
    *is_supported = format_supported;
    return format_result;
}

CupError layout_create_staging_dir(char *buffer,
                                   size_t size,
                                   const char *operation,
                                   const PackageIdentity *identity) {
    TEST_ASSERT_TRUE(strcmp(operation, "install") == 0 || strcmp(operation, "update") == 0);
    TEST_ASSERT_NOT_NULL(identity);
    if (tmp_result != CUP_OK) {
        return tmp_result;
    }
    return snprintf(buffer, size, "/tmp/staging") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    if (install_path_result != CUP_OK) {
        return install_path_result;
    }
    return snprintf(buffer, size, "/tmp/install") > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path) {
    TEST_ASSERT_TRUE(operation == PACKAGE_OPERATION_INSTALL ||
                     operation == PACKAGE_OPERATION_UPDATE);
    TEST_ASSERT_NOT_NULL(package);
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", temporary_path);
    return begin_result;
}

CupError package_catalog_build_url(const PackageCatalog *catalog,
                                   char *buffer,
                                   size_t size,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   const char *format) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)version;
    (void)format;
    if (url_result != CUP_OK) {
        return url_result;
    }
    return snprintf(buffer, size, "https://example.invalid/package") > 0 ? CUP_OK
                                                                         : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_catalog_build_checksum_url(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform,
                                            const char *version) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    (void)version;
    if (checksum_url_result != CUP_OK) {
        return checksum_url_result;
    }
    return snprintf(buffer, size, "https://example.invalid/SHA256SUMS") > 0
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_cache_fetch(char *archive_path,
                             size_t archive_path_size,
                             const char *package_url,
                             const char *checksum_url,
                             const PackageIdentity *identity,
                             const char *format,
                             PackageCachePolicy cache_policy,
                             PackageCacheSource *source) {
    int index = fetch_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    TEST_ASSERT_NOT_NULL(package_url);
    TEST_ASSERT_NOT_NULL(checksum_url);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(format);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_INT(index == 0 ? PACKAGE_CACHE_ALLOW : PACKAGE_CACHE_REFRESH, cache_policy);
    if (fetch_results[index] != CUP_OK) {
        return fetch_results[index];
    }
    snprintf(archive_path, archive_path_size, "/tmp/archive.tar.gz");
    *source = fetch_sources[index];
    return CUP_OK;
}

int interrupt_requested(void) {
    int index = interrupt_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return interrupt_values[index];
}

CupError package_extract_archive(const char *archive_path,
                                 const char *tmp_path,
                                 const char *format) {
    int index = extract_calls++;
    TEST_ASSERT_EQUAL_STRING("/tmp/archive.tar.gz", archive_path);
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", tmp_path);
    TEST_ASSERT_EQUAL_STRING("tar.gz", format);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return extract_results[index];
}

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    int index = validate_calls++;
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", base_path);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return validate_results[index];
}

CupError package_cache_discard(const char *archive_path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/archive.tar.gz", archive_path);
    discard_calls++;
    return discard_result;
}

CupError filesystem_remove_tree(const char *path) {
    int index = remove_calls++;
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", path);
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return remove_results[index];
}

CupError filesystem_ensure_directory(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", path);
    ensure_dir_calls++;
    return ensure_dir_result;
}

CupError package_set_metadata_read_only(const char *base_path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", base_path);
    return read_only_result;
}

CupError layout_ensure_package_parent(const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    return parent_result;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    int index = move_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(commit_state);
    *commit_state = move_states[index];
    return move_results[index];
}

CupError state_add_installed(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("compiler", identity->component);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return add_state_result;
}

const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope) {
    static PackageIdentity identity;
    char tool[MAX_IDENTIFIER_LEN];
    char version[MAX_IDENTIFIER_LEN];

    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(scope);
    if (current_active == NULL) {
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_selector_parse_parts(current_active, tool, sizeof(tool), version, sizeof(version)));
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.component, scope->component);
    strcpy(identity.tool, tool);
    strcpy(identity.host_platform, scope->host_platform);
    strcpy(identity.target_platform, scope->target_platform);
    strcpy(identity.version, version);
    return &identity;
}

CupError state_set_active(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("compiler", identity->component);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return set_active_result;
}

CupError package_identity_validate(const PackageIdentity *identity) {
    return identity == NULL || identity->tool[0] == '\0' || identity->version[0] == '\0'
               ? CUP_ERR_INVALID_INPUT
               : CUP_OK;
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

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    TEST_ASSERT_NOT_NULL(identity);
    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    TEST_ASSERT_NOT_NULL(plan);
    TEST_ASSERT_NOT_NULL(state);
    plan_build_calls++;
    return plan_build_result;
}

CupError state_save(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    save_calls++;
    return save_result;
}

CupError package_transaction_clear(void) {
    int index = clear_calls++;
    TEST_ASSERT_TRUE(index < MAX_STEPS);
    return clear_results[index];
}

CupError wrapper_plan_apply(const WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_apply_calls++;
    return plan_apply_result;
}

void wrapper_plan_init(WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    memset(plan, 0, sizeof(*plan));
    plan_init_calls++;
}

void wrapper_plan_free(WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_free_calls++;
}

void package_request_print(FILE *stream, const PackageRequest *request) {
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_NOT_NULL(request);
    fputs(request->resolved_selector, stream);
}

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release) {
    if (entry_build_result != CUP_OK) {
        return entry_build_result;
    }
    return snprintf(buffer, size, "%s@%s", tool, release) > 0 ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError package_selector_parse_parts(
    const char *entry, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *separator;
    size_t length;

    if (entry_parse_result != CUP_OK) {
        return entry_parse_result;
    }
    separator = strchr(entry, '@');
    if (separator == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    length = (size_t)(separator - entry);
    if (length >= tool_size || strlen(separator + 1) >= release_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(tool, entry, length);
    tool[length] = '\0';
    strcpy(release, separator + 1);
    return CUP_OK;
}

static void assert_cleanup(void) {
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_init_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_free_calls);
}

static void test_public_inputs(void) {
    int installed;
    int moved;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_install_update_scope(NULL, "clang", NULL, NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_install_update_scope("compiler", "", NULL, NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_install_update_scope("compiler", "clang", NULL, NULL, NULL, &moved));

    entry_build_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_BUFFER_TOO_SMALL,
        package_install_update_scope("compiler", "clang", NULL, NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(0, plan_init_calls);

    reset_scenario();
    {
        char long_active[MAX_SELECTOR_LEN + 8];
        memset(long_active, 'x', sizeof(long_active) - 1);
        long_active[sizeof(long_active) - 1] = '\0';
        TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                              package_install_update_scope(
                                  "compiler", "clang", NULL, long_active, &installed, &moved));
    }
}

static void test_prepare_failures(void) {
    parse_result = CUP_ERR_INVALID_INPUT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_install("compiler", "bad", NULL, NULL));
    assert_cleanup();

    reset_scenario();
    context_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    guard_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    resolve_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    identity_result = CUP_ERR_INVALID_RELEASE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    version_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    version_available = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    absent_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ALREADY_INSTALLED,
                          package_install("compiler", "clang@stable", NULL, NULL));
}

static void test_update_guards(void) {
    int installed = -1;
    int moved = -1;

    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_NOT_INSTALLED,
        package_install_update_scope("compiler", "clang", NULL, NULL, &installed, &moved));
    TEST_ASSERT_EQUAL_INT(0, installed);
    TEST_ASSERT_EQUAL_INT(0, moved);

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "broken");
    entry_parse_result = CUP_ERR_INVALID_INPUT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INCONSISTENT_STATE,
        package_install_update_scope("compiler", "clang", NULL, NULL, &installed, &moved));

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    valid_installed_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_VALIDATION,
        package_install_update_scope("compiler", "clang", NULL, NULL, &installed, &moved));

    reset_scenario();
    add_entry("compiler", "macos-x64", "linux-x64", "clang@1.0.0");
    add_entry("compiler", "linux-x64", "windows-x64", "clang@1.0.0");
    add_entry("debugger", "linux-x64", "linux-x64", "gdb@1.0.0");
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_install_update_scope("compiler", "clang", NULL, NULL, &installed, &moved));
}

static void test_format_selection(void) {
    char long_format[MAX_IDENTIFIER_LEN + 8];

    default_format_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    memset(long_format, 'x', sizeof(long_format) - 1);
    long_format[sizeof(long_format) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_install("compiler", "clang@stable", NULL, long_format));

    reset_scenario();
    format_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG,
                          package_install("compiler", "clang@stable", NULL, "zip"));

    reset_scenario();
    format_supported = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_install("compiler", "clang@stable", NULL, "zip"));

    reset_scenario();
    tmp_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY,
                          package_install("compiler", "clang@stable", NULL, "tar.gz"));

    reset_scenario();
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, remove_calls);

    reset_scenario();
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    begin_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, remove_calls);
}

static void test_cache_refresh(void) {
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, fetch_calls);
    TEST_ASSERT_EQUAL_INT(2, extract_calls);
    TEST_ASSERT_EQUAL_INT(1, discard_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, ensure_dir_calls);

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    discard_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    fetch_sources[0] = PACKAGE_CACHE_SOURCE_CACHE;
    extract_results[0] = CUP_ERR_ARCHIVE;
    ensure_dir_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    validate_results[0] = CUP_ERR_VALIDATION;
    discard_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, discard_calls);
}

static void test_fetch_failures(void) {
    url_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    checksum_url_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    fetch_results[0] = CUP_ERR_FETCH;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FETCH, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    interrupt_values[0] = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    interrupt_values[1] = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INTERRUPT,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    read_only_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    parent_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));
}

static void test_new_install_commit(void) {
    int installed = 0;
    int moved = 0;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_active = "clang@1.0.0";
    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_install_update_scope("compiler", "clang", NULL, "clang@1.0.0", &installed, &moved));
    TEST_ASSERT_EQUAL_INT(0, installed);
    TEST_ASSERT_EQUAL_INT(1, moved);
    TEST_ASSERT_EQUAL_INT(0, fetch_calls);
    TEST_ASSERT_EQUAL_INT(1, save_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);
}

static void test_commit_failures(void) {
    move_results[0] = CUP_ERR_FILESYSTEM;
    move_states[0] = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, remove_calls);
    TEST_ASSERT_EQUAL_INT(1, clear_calls);

    reset_scenario();
    move_results[0] = CUP_ERR_FILESYSTEM;
    move_states[0] = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, remove_calls);

    reset_scenario();
    add_state_result = CUP_ERR_STATE_FULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_FULL,
                          package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    set_active_result = CUP_ERR_ACTIVE_FULL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ACTIVE_FULL,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    plan_build_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    save_result = CUP_ERR_STATE_SAVE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_SAVE,
                          package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    save_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_install("compiler", "clang@stable", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, move_calls);

    reset_scenario();
    clear_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    plan_apply_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_active = "clang@1.0.0";
    set_active_result = CUP_ERR_ACTIVE_FULL;
    {
        int installed;
        int moved;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_ACTIVE_FULL,
                              package_install_update_scope(
                                  "compiler", "clang", NULL, "clang@1.0.0", &installed, &moved));
    }

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_active = "clang@1.0.0";
    save_result = CUP_ERR_STATE_SAVE;
    {
        int installed;
        int moved;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_SAVE,
                              package_install_update_scope(
                                  "compiler", "clang", NULL, "clang@1.0.0", &installed, &moved));
    }

    reset_scenario();
    add_entry("compiler", "linux-x64", "linux-x64", "clang@1.0.0");
    absent_result = CUP_ERR_ALREADY_INSTALLED;
    current_active = "clang@1.0.0";
    plan_apply_result = CUP_ERR_FILESYSTEM;
    {
        int installed;
        int moved;
        TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                              package_install_update_scope(
                                  "compiler", "clang", NULL, "clang@1.0.0", &installed, &moved));
    }
}

static void test_rollback_failures(void) {
    add_state_result = CUP_ERR_STATE_FULL;
    move_results[1] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    extract_results[0] = CUP_ERR_EXTRACT;
    remove_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK,
                          package_install("compiler", "clang@stable", NULL, NULL));

    reset_scenario();
    extract_results[0] = CUP_ERR_EXTRACT;
    clear_results[0] = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK,
                          package_install("compiler", "clang@stable", NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_public_inputs);
    RUN_TEST(test_prepare_failures);
    RUN_TEST(test_update_guards);
    RUN_TEST(test_format_selection);
    RUN_TEST(test_cache_refresh);
    RUN_TEST(test_fetch_failures);
    RUN_TEST(test_new_install_commit);
    RUN_TEST(test_commit_failures);
    RUN_TEST(test_rollback_failures);
    return UNITY_END();
}
