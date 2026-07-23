/*
 * Test focus: Exercises command-context setup, lock/root preconditions, entry resolution and
 * installed-package requirements through boundary stubs.
 */

#include "cup_assets.h"
#include "command_context.h"
#include "installed_package.h"
#include "package_selector.h"
#include "package_request.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "platform.h"
#include "state.h"
#include "system.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static CupError pending_result;
static int uninstall_pending;
static int uninstall_pending_after_lock;
static size_t uninstall_pending_calls;
static CupError host_result;
static char host_value[MAX_PLATFORM_LEN];
static CupError platform_validation_result;
static CupError runtime_result;
static LayoutRuntimeStatus runtime_statuses[2];
static size_t runtime_calls;
static CupError cup_assets_result;
static int installed_cup_assets_valid;
static int development_cup_assets_valid;
static CupError ensure_root_result;
static int ensure_root_calls;
static CupError lock_path_result;
static CupError lock_result;
static int lock_acquire_calls;
static SystemLockMode acquired_mode;
static int lock_release_calls;
static CupError ensure_runtime_result;
static CupError root_path_result;
static CupError state_load_result;
static StateFileStatus state_file_status;
static CupError state_validation_result;
static CupError state_save_result;
static int state_save_calls;
static int state_has_package;
static CupError package_catalog_load_result;
static CupError stable_result;
static char stable_value[MAX_IDENTIFIER_LEN];
static int package_catalog_init_calls;
static int package_catalog_free_calls;
static CupError package_presence_result;
static int package_on_disk;
static CupError install_path_result;
static CupError package_validation_result;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    pending_result = CUP_OK;
    uninstall_pending = 0;
    uninstall_pending_after_lock = 0;
    uninstall_pending_calls = 0;
    host_result = CUP_OK;
    strcpy(host_value, "linux-x64");
    platform_validation_result = CUP_OK;
    runtime_result = CUP_OK;
    runtime_statuses[0] = LAYOUT_RUNTIME_READY;
    runtime_statuses[1] = LAYOUT_RUNTIME_READY;
    runtime_calls = 0;
    cup_assets_result = CUP_OK;
    installed_cup_assets_valid = 1;
    development_cup_assets_valid = 0;
    ensure_root_result = CUP_OK;
    ensure_root_calls = 0;
    lock_path_result = CUP_OK;
    lock_result = CUP_OK;
    lock_acquire_calls = 0;
    acquired_mode = SYSTEM_LOCK_SHARED;
    lock_release_calls = 0;
    ensure_runtime_result = CUP_OK;
    root_path_result = CUP_OK;
    state_load_result = CUP_OK;
    state_file_status = STATE_FILE_LOADED;
    state_validation_result = CUP_OK;
    state_save_result = CUP_OK;
    state_save_calls = 0;
    state_has_package = 0;
    package_catalog_load_result = CUP_OK;
    stable_result = CUP_OK;
    strcpy(stable_value, "22.1.5");
    package_catalog_init_calls = 0;
    package_catalog_free_calls = 0;
    package_presence_result = CUP_OK;
    package_on_disk = 0;
    install_path_result = CUP_OK;
    package_validation_result = CUP_OK;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}
static void read_stream_text(FILE *stream, char *output, size_t output_size) {
    size_t read_count;

    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_GREATER_THAN(0, output_size);
    TEST_ASSERT_EQUAL_INT(0, fflush(stream));
    TEST_ASSERT_EQUAL_INT(0, fseek(stream, 0, SEEK_SET));
    read_count = fread(output, 1, output_size - 1, stream);
    TEST_ASSERT_FALSE(ferror(stream));
    output[read_count] = '\0';
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError cup_assets_uninstall_is_pending(int *pending) {
    if (pending == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    uninstall_pending_calls++;
    *pending = uninstall_pending_calls > 1 && uninstall_pending_after_lock ? 1 : uninstall_pending;
    return pending_result;
}

CupError platform_get_host(char *buffer, size_t size) {
    if (host_result != CUP_OK) {
        return host_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", host_value), size);
}

CupError platform_validate(const char *platform) {
    TEST_ASSERT_NOT_NULL(platform);
    return platform_validation_result;
}

CupError layout_get_runtime_status(LayoutRuntimeStatus *status) {
    if (runtime_result != CUP_OK) {
        return runtime_result;
    }
    TEST_ASSERT_NOT_NULL(status);
    *status = runtime_statuses[runtime_calls < 2 ? runtime_calls : 1];
    runtime_calls++;
    return CUP_OK;
}

CupError cup_assets_inspect(CupAssetsInspection *inspection) {
    TEST_ASSERT_NOT_NULL(inspection);
    memset(inspection, 0, sizeof(*inspection));
    return cup_assets_result;
}

int cup_assets_installed_is_valid(const CupAssetsInspection *inspection) {
    TEST_ASSERT_NOT_NULL(inspection);
    return installed_cup_assets_valid;
}

int cup_assets_development_is_valid(const CupAssetsInspection *inspection) {
    TEST_ASSERT_NOT_NULL(inspection);
    return development_cup_assets_valid;
}

CupError layout_ensure_root(void) {
    ensure_root_calls++;
    return ensure_root_result;
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    if (lock_path_result != CUP_OK) {
        return lock_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/cup.lock"), size);
}

CupError system_lock_acquire(SystemLock *lock, const char *path, SystemLockMode mode) {
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_NOT_NULL(path);
    lock_acquire_calls++;
    acquired_mode = mode;
    if (lock_result == CUP_OK) {
        lock->active = 1;
    }
    return lock_result;
}

void system_lock_release(SystemLock *lock) {
    if (lock != NULL && lock->active) {
        lock->active = 0;
        lock_release_calls++;
    }
}

CupError layout_ensure_runtime(void) {
    return ensure_runtime_result;
}

CupError state_save(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    state_save_calls++;
    return state_save_result;
}

CupError layout_get_root(char *buffer, size_t size) {
    if (root_path_result != CUP_OK) {
        return root_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/.cup"), size);
}

void package_catalog_init(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    memset(catalog, 0, sizeof(*catalog));
    package_catalog_init_calls++;
}

void package_catalog_free(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    catalog->packages = NULL;
    catalog->count = 0;
    package_catalog_free_calls++;
}

CupError state_load(CupState *state, StateFileStatus *status) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(status);
    memset(state, 0, sizeof(*state));
    *status = state_file_status;
    return state_load_result;
}

CupError state_validate(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    return state_validation_result;
}

CupError state_validate_current_host(const CupState *state, const char *current_host) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(current_host);
    return CUP_OK;
}

CupError package_catalog_load(PackageCatalog *catalog) {
    TEST_ASSERT_NOT_NULL(catalog);
    return package_catalog_load_result;
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform) {
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(tool);
    TEST_ASSERT_NOT_NULL(host_platform);
    TEST_ASSERT_NOT_NULL(target_platform);
    if (stable_result != CUP_OK) {
        return stable_result;
    }
    return buffer_write_result(snprintf(buffer, size, "%s", stable_value), size);
}

int state_find_installed(const CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    return state_has_package ? 0 : -1;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    TEST_ASSERT_NOT_NULL(identity);
    return package_selector_format_parts(buffer, size, identity->tool, identity->version);
}

CupError package_path_exists(const PackageIdentity *identity, int *exists) {
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_NOT_NULL(exists);
    *exists = package_on_disk;
    return package_presence_result;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    if (install_path_result != CUP_OK) {
        return install_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/package"), size);
}

CupError package_validate(const char *base_path, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(base_path);
    TEST_ASSERT_NOT_NULL(identity);
    return package_validation_result;
}

static PackageIdentity sample_package(void) {
    PackageIdentity package;
    memset(&package, 0, sizeof(package));
    strcpy(package.component, "compiler");
    strcpy(package.tool, "clang");
    strcpy(package.host_platform, "linux-x64");
    strcpy(package.target_platform, "linux-x64");
    strcpy(package.version, "22.1.5");
    return package;
}

/*
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_invalid_context(void) {
    CommandContext context;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          command_context_begin(NULL, NULL, SYSTEM_LOCK_SHARED));

    pending_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    uninstall_pending = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    host_result = CUP_ERR_INVALID_OS;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_OS,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    platform_validation_result = CUP_ERR_INVALID_OS;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_OS,
                          command_context_begin(&context, "bad", SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_INCOMPLETE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
}

static void test_marker_after_lock(void) {
    CommandContext context;

    uninstall_pending_after_lock = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(2, (int)uninstall_pending_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_missing_runtime(void) {
    CommandContext context;

    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    cup_assets_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    installed_cup_assets_valid = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    installed_cup_assets_valid = 0;
    development_cup_assets_valid = 1;
    ensure_root_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    lock_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    lock_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, acquired_mode);
}

static void test_runtime_recheck(void) {
    CommandContext context;

    runtime_statuses[1] = LAYOUT_RUNTIME_INCOMPLETE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    runtime_statuses[1] = LAYOUT_RUNTIME_MISSING;
    ensure_runtime_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    runtime_statuses[1] = LAYOUT_RUNTIME_MISSING;
    state_save_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT,
                          command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_INT(1, state_save_calls);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    runtime_statuses[1] = LAYOUT_RUNTIME_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_STRING("linux-x64", context.host_platform);
    TEST_ASSERT_EQUAL_STRING("linux-x64", context.target_platform);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, acquired_mode);
    TEST_ASSERT_EQUAL_INT(1, state_save_calls);
    command_context_end(&context);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
    TEST_ASSERT_EQUAL_INT(1, package_catalog_free_calls);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          command_context_begin(&context, "windows-x64", SYSTEM_LOCK_SHARED));
    TEST_ASSERT_EQUAL_STRING("windows-x64", context.target_platform);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_SHARED, acquired_mode);
    command_context_end(&context);
    command_context_end(NULL);
}

static void test_read_only_context(void) {
    CommandContext context;

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_context_begin_read_only(NULL, NULL));

    runtime_statuses[0] = LAYOUT_RUNTIME_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_begin_read_only(&context, "WINDOWS-X64"));
    TEST_ASSERT_FALSE(context.runtime_available);
    TEST_ASSERT_EQUAL_STRING("windows-x64", context.target_platform);
    TEST_ASSERT_EQUAL_INT(0, ensure_root_calls);
    TEST_ASSERT_EQUAL_INT(0, lock_acquire_calls);
    command_context_end(&context);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_begin_read_only(&context, NULL));
    TEST_ASSERT_TRUE(context.runtime_available);
    TEST_ASSERT_EQUAL_INT(1, lock_acquire_calls);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_SHARED, acquired_mode);
    command_context_end(&context);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);

    reset_scenario();
    runtime_statuses[1] = LAYOUT_RUNTIME_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_begin_read_only(&context, NULL));
    TEST_ASSERT_FALSE(context.runtime_available);
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
    TEST_ASSERT_EQUAL_INT(0, ensure_root_calls);

    reset_scenario();
    runtime_statuses[1] = LAYOUT_RUNTIME_INCOMPLETE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_context_begin_read_only(&context, NULL));
    TEST_ASSERT_EQUAL_INT(1, lock_release_calls);
}

static void test_load_contracts(void) {
    CommandContext context;
    memset(&context, 0, sizeof(context));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_context_load_state(NULL));
    state_load_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_context_load_state(&context));

    reset_scenario();
    state_file_status = STATE_FILE_MISSING;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_context_load_state(&context));

    reset_scenario();
    state_validation_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE, command_context_load_state(&context));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_load_state(&context));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, command_context_load_catalog(NULL));
    package_catalog_load_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_context_load_catalog(&context));
    TEST_ASSERT_FALSE(context.has_catalog);

    package_catalog_load_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_context_load_catalog(&context));
    TEST_ASSERT_TRUE(context.has_catalog);

    context.has_catalog = 0;
    package_catalog_load_result = CUP_ERR_CATALOG;
    command_context_try_catalog(&context);
    TEST_ASSERT_FALSE(context.has_catalog);
    package_catalog_load_result = CUP_OK;
    command_context_try_catalog(&context);
    TEST_ASSERT_TRUE(context.has_catalog);
    command_context_try_catalog(NULL);
}

static void test_entry_requests(void) {
    PackageRequest request;
    PackageCatalog catalog = {0};
    FILE *stream;
    char output[128];

    /* Parsing rejects incomplete, unsupported, and unsafe selectors. */
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse(NULL, "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, package_request_parse("compiler", NULL, &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse("compiler", "clang@stable", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT,
                          package_request_parse("invalid", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse("compiler", "clang", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          package_request_parse("compiler", "lldb@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_request_parse("compiler", "clang@../bad", &request));

    /* Stable requests require a catalog entry and a valid concrete result. */
    TEST_ASSERT_EQUAL_INT(CUP_OK, package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT,
        package_request_resolve(&catalog, "compiler", "linux-x64", "linux-x64", NULL));
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_CATALOG,
        package_request_resolve(NULL, "compiler", "linux-x64", "linux-x64", &request));

    stable_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_NOT_AVAILABLE,
        package_request_resolve(&catalog, "compiler", "linux-x64", "linux-x64", &request));

    stable_result = CUP_OK;
    strcpy(stable_value, "../bad");
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_RELEASE,
        package_request_resolve(&catalog, "compiler", "linux-x64", "linux-x64", &request));

    strcpy(stable_value, "22.1.5");
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_request_resolve(&catalog, "compiler", "linux-x64", "linux-x64", &request));
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", request.resolved_selector);

    /* Printed requests expose stable resolution while concrete requests remain unchanged. */
    stream = tmpfile();
    TEST_ASSERT_NOT_NULL(stream);
    package_request_print(stream, &request);
    read_stream_text(stream, output, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("clang@stable -> clang@22.1.5", output);
    TEST_ASSERT_EQUAL_INT(0, fclose(stream));

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_request_parse("compiler", "clang@22.1.5", &request));
    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_request_resolve(NULL, "compiler", "linux-x64", "linux-x64", &request));
    stream = tmpfile();
    TEST_ASSERT_NOT_NULL(stream);
    package_request_print(stream, &request);
    read_stream_text(stream, output, sizeof(output));
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", output);
    TEST_ASSERT_EQUAL_INT(0, fclose(stream));
    package_request_print(NULL, &request);
    package_request_print(stdout, NULL);
}

static void test_package_guards(void) {
    CommandContext context;
    PackageIdentity package = sample_package();
    memset(&context, 0, sizeof(context));

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, installed_package_require_present(NULL, &package));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          installed_package_require_present(&context.state, NULL));

    package_presence_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM,
                          installed_package_require_present(&context.state, &package));

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED,
                          installed_package_require_present(&context.state, &package));
    TEST_ASSERT_EQUAL_INT(CUP_OK, installed_package_require_absent(&context.state, &package));

    state_has_package = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          installed_package_require_present(&context.state, &package));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          installed_package_require_absent(&context.state, &package));

    package_on_disk = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, installed_package_require_present(&context.state, &package));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ALREADY_INSTALLED,
                          installed_package_require_absent(&context.state, &package));

    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          installed_package_require_valid(&context.state, &package));

    install_path_result = CUP_OK;
    package_validation_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          installed_package_require_valid(&context.state, &package));

    package_validation_result = CUP_OK;
    TEST_ASSERT_EQUAL_INT(CUP_OK, installed_package_require_valid(&context.state, &package));

    state_has_package = 0;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          installed_package_require_absent(&context.state, &package));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_context);
    RUN_TEST(test_marker_after_lock);
    RUN_TEST(test_missing_runtime);
    RUN_TEST(test_runtime_recheck);
    RUN_TEST(test_read_only_context);
    RUN_TEST(test_load_contracts);
    RUN_TEST(test_entry_requests);
    RUN_TEST(test_package_guards);
    return UNITY_END();
}
