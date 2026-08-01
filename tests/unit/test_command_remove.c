/*
 * Test focus: Exercises remove preparation, state commit, rollback uncertainty and entry-point
 * reconciliation without duplicating the real command state machine.
 */

#include "command_context.h"
#include "installed_package.h"
#include "commands.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "registry.h"
#include "text.h"
#include "unity.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Scenario controls and observations. Configured results drive the boundary doubles below;
 * counters record the calls made by production code.
 */

static CupError parse_result;
static CupError context_result;
static CupError transaction_check_result;
static CupError load_state_result;
static CupError load_catalog_result;
static CupError resolve_result;
static CupError identity_result;
static CupError installed_result;
static CupError install_path_result;
static CupError temp_path_result;
static CupError transaction_begin_result;
static CupError initial_move_result;
static SystemCommitState initial_move_state;
static CupError rollback_move_result;
static const char *default_entry;
static CupError clear_active_result;
static CupError remove_installed_result;
static CupError plan_build_result;
static CupError state_save_result;
static CupError remove_tree_result;
static CupError transaction_clear_result;
static CupError plan_apply_result;
static int stable_request;
static int context_end_calls;
static int plan_free_calls;
static int move_calls;
static int transaction_clear_calls;
static int remove_tree_calls;
static int plan_build_calls;
static int plan_apply_calls;
static size_t installed_match_count;

/* Fixture lifecycle and local construction helpers. */

static CupError buffer_write_result(int written, size_t size) {
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static void reset_scenario(void) {
    parse_result = CUP_OK;
    context_result = CUP_OK;
    transaction_check_result = CUP_OK;
    load_state_result = CUP_OK;
    load_catalog_result = CUP_OK;
    resolve_result = CUP_OK;
    identity_result = CUP_OK;
    installed_result = CUP_OK;
    install_path_result = CUP_OK;
    temp_path_result = CUP_OK;
    transaction_begin_result = CUP_OK;
    initial_move_result = CUP_OK;
    initial_move_state = SYSTEM_COMMIT_DURABLE;
    rollback_move_result = CUP_OK;
    default_entry = NULL;
    clear_active_result = CUP_OK;
    remove_installed_result = CUP_OK;
    plan_build_result = CUP_OK;
    state_save_result = CUP_OK;
    remove_tree_result = CUP_OK;
    transaction_clear_result = CUP_OK;
    plan_apply_result = CUP_OK;
    stable_request = 0;
    context_end_calls = 0;
    plan_free_calls = 0;
    move_calls = 0;
    transaction_clear_calls = 0;
    remove_tree_calls = 0;
    plan_build_calls = 0;
    plan_apply_calls = 0;
    installed_match_count = 0;
}

void setUp(void) {
    reset_scenario();
}

void tearDown(void) {
}

/*
 * Controlled boundary doubles. Each implementation exposes one dependency through the scenario
 * state above.
 */

CupError package_request_parse(const char *component, const char *entry, PackageRequest *request) {
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_NOT_NULL(request);
    if (parse_result != CUP_OK) {
        return parse_result;
    }
    memset(request, 0, sizeof(*request));
    strcpy(request->selector.tool, "clang");
    strcpy(request->selector.release, stable_request ? "stable" : "22.1.5");
    strcpy(request->input_selector, stable_request ? "clang@stable" : "clang@22.1.5");
    return CUP_OK;
}

int text_is_empty(const char *value) {
    return value == NULL || value[0] == '\0';
}

CupError text_copy(char *destination, size_t size, const char *source) {
    size_t length = strlen(source);

    if (length >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memmove(destination, source, length + 1);
    return CUP_OK;
}

CupError text_copy_lower_ascii(char *destination, size_t size, const char *source) {
    size_t i;
    CupError err = text_copy(destination, size, source);

    if (err != CUP_OK) {
        return err;
    }
    for (i = 0; destination[i] != '\0'; ++i) {
        if (destination[i] >= 'A' && destination[i] <= 'Z') {
            destination[i] = (char)(destination[i] - 'A' + 'a');
        }
    }
    return CUP_OK;
}

CupError package_selector_parse_parts(
    const char *text, char *tool, size_t tool_size, char *release, size_t release_size) {
    const char *separator = text == NULL ? NULL : strchr(text, '@');
    size_t tool_length;

    if (separator == NULL || separator == text || separator[1] == '\0' ||
        strchr(separator + 1, '@') != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    tool_length = (size_t)(separator - text);
    if (tool_length >= tool_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(tool, text, tool_length);
    tool[tool_length] = '\0';
    return text_copy(release, release_size, separator + 1);
}

CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release) {
    return buffer_write_result(snprintf(buffer, size, "%s@%s", tool, release), size);
}

CupError registry_validate_tool(const char *component, const char *tool) {
    return strcmp(component, "compiler") == 0 && strcmp(tool, "clang") == 0
               ? CUP_OK
               : CUP_ERR_INVALID_TOOL;
}

CupError registry_find_tool_component(const char *tool, char *component, size_t component_size) {
    if (strcmp(tool, "clang") != 0) {
        return CUP_ERR_INVALID_TOOL;
    }
    return text_copy(component, component_size, "compiler");
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    (void)target_override;
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SYSTEM_LOCK_EXCLUSIVE, mode);
    memset(context, 0, sizeof(*context));
    if (context_result == CUP_OK) {
        strcpy(context->host_platform, "linux-x64");
        strcpy(context->target_platform, "linux-x64");
    }
    return context_result;
}

void command_context_end(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    context_end_calls++;
}

CupError runtime_journal_require_none(void) {
    return transaction_check_result;
}

CupError command_context_load_state(CommandContext *context) {
    size_t i;

    TEST_ASSERT_NOT_NULL(context);
    for (i = 0; i < installed_match_count; ++i) {
        PackageIdentity *identity = &context->state.installed[i];

        strcpy(identity->component, "compiler");
        strcpy(identity->tool, "clang");
        strcpy(identity->host_platform, "linux-x64");
        strcpy(identity->target_platform, "linux-x64");
        strcpy(identity->version, i == 0 ? "21.1.5" : "22.1.5");
    }
    context->state.installed_count = installed_match_count;
    return load_state_result;
}

int package_release_is_stable(const char *release) {
    return release != NULL && strcmp(release, "stable") == 0;
}

CupError command_context_load_catalog(CommandContext *context) {
    TEST_ASSERT_NOT_NULL(context);
    if (load_catalog_result == CUP_OK) {
        context->has_catalog = 1;
    }
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

CupError installed_package_require_present(const CupState *state, const PackageIdentity *package) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(package);
    return installed_result;
}

CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(identity);
    if (install_path_result != CUP_OK) {
        return install_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/install"), size);
}

CupError layout_make_staging_path(char *buffer,
                                  size_t size,
                                  const char *operation,
                                  const PackageIdentity *identity) {
    TEST_ASSERT_EQUAL_STRING("remove", operation);
    TEST_ASSERT_NOT_NULL(identity);
    if (temp_path_result != CUP_OK) {
        return temp_path_result;
    }
    return buffer_write_result(snprintf(buffer, size, "/tmp/staging"), size);
}

CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path) {
    TEST_ASSERT_EQUAL_INT(PACKAGE_OPERATION_REMOVE, operation);
    TEST_ASSERT_NOT_NULL(package);
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", temporary_path);
    return transaction_begin_result;
}

CupError system_move_path(const char *source,
                          const char *destination,
                          SystemCommitState *commit_state) {
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(commit_state);
    move_calls++;
    if (move_calls == 1) {
        *commit_state = initial_move_state;
        return initial_move_result;
    }
    *commit_state =
        rollback_move_result == CUP_OK ? SYSTEM_COMMIT_DURABLE : SYSTEM_COMMIT_NOT_APPLIED;
    return rollback_move_result;
}

const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope) {
    static PackageIdentity identity;
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(scope);
    if (default_entry == NULL) {
        return NULL;
    }
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", default_entry);
    memset(&identity, 0, sizeof(identity));
    strcpy(identity.component, scope->component);
    strcpy(identity.tool, "clang");
    strcpy(identity.host_platform, scope->host_platform);
    strcpy(identity.target_platform, scope->target_platform);
    strcpy(identity.version, "22.1.5");
    return &identity;
}

CupError state_clear_matching_active(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return clear_active_result;
}

CupError state_remove_installed(CupState *state, const PackageIdentity *identity) {
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(identity);
    TEST_ASSERT_EQUAL_STRING("clang", identity->tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", identity->version);
    return remove_installed_result;
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

CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state) {
    TEST_ASSERT_NOT_NULL(plan);
    TEST_ASSERT_NOT_NULL(state);
    plan_build_calls++;
    return plan_build_result;
}

CupError state_save(const CupState *state) {
    TEST_ASSERT_NOT_NULL(state);
    return state_save_result;
}

CupError filesystem_remove_tree(const char *path) {
    TEST_ASSERT_EQUAL_STRING("/tmp/staging", path);
    remove_tree_calls++;
    return remove_tree_result;
}

CupError runtime_journal_clear(void) {
    transaction_clear_calls++;
    return transaction_clear_result;
}

CupError wrapper_plan_apply(const WrapperPlan *plan) {
    TEST_ASSERT_NOT_NULL(plan);
    plan_apply_calls++;
    return plan_apply_result;
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

static void assert_common_cleanup(void) {
    TEST_ASSERT_EQUAL_INT(1, context_end_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_free_calls);
}

static char *capture_ambiguous_remove(CupError *result) {
    FILE *capture = tmpfile();
    int saved;
    int capture_flush_result;
    int restore_result;
    int close_result;
    long length;
    char *output;

    TEST_ASSERT_NOT_NULL(capture);
    TEST_ASSERT_EQUAL_INT(0, fflush(stderr));
    saved = test_dup_fd(TEST_PLATFORM_STDERR_FD);
    TEST_ASSERT_TRUE(saved >= 0);
    TEST_ASSERT_TRUE(test_dup2_fd(test_file_descriptor(capture), TEST_PLATFORM_STDERR_FD) >= 0);

    *result = command_remove(NULL, "clang", NULL);

    capture_flush_result = fflush(stderr);
    restore_result = test_dup2_fd(saved, TEST_PLATFORM_STDERR_FD);
    close_result = test_close_fd(saved);
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
 * Test cases exercise the real production entry point while changing only controlled boundary
 * outcomes.
 */

static void test_remove_prepare_fail(void) {
    parse_result = CUP_ERR_INVALID_INPUT;
    TEST_ASSERT_EQUAL_INT(
        CUP_ERR_INVALID_INPUT, command_remove("compiler", "clang@22.1.5", NULL));
    assert_common_cleanup();

    reset_scenario();
    context_result = CUP_ERR_LOCK;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_LOCK, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    transaction_check_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    load_state_result = CUP_ERR_STATE_LOAD;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    stable_request = 1;
    load_catalog_result = CUP_ERR_CATALOG;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG, command_remove("compiler", "clang@stable", NULL));

    reset_scenario();
    resolve_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    identity_result = CUP_ERR_INVALID_RELEASE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    installed_result = CUP_ERR_NOT_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    install_path_result = CUP_ERR_BUFFER_TOO_SMALL;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    temp_path_result = CUP_ERR_TEMPORARY;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TEMPORARY, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    transaction_begin_result = CUP_ERR_TRANSACTION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_TRANSACTION, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    transaction_begin_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(0, move_calls);
}

static void test_staging_rollback(void) {
    initial_move_result = CUP_ERR_FILESYSTEM;
    initial_move_state = SYSTEM_COMMIT_NOT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);
    assert_common_cleanup();
}

static void test_uncertain_staging(void) {
    initial_move_result = CUP_ERR_FILESYSTEM;
    initial_move_state = SYSTEM_COMMIT_APPLIED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_EQUAL_INT(0, transaction_clear_calls);
}

static void test_safe_rollback(void) {
    clear_active_result = CUP_ERR_INCONSISTENT_STATE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(2, move_calls);
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);

    reset_scenario();
    remove_installed_result = CUP_ERR_NOT_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    default_entry = "clang@22.1.5";
    plan_build_result = CUP_ERR_VALIDATION;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_VALIDATION, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(2, move_calls);

    reset_scenario();
    state_save_result = CUP_ERR_STATE_SAVE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_SAVE, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(2, move_calls);
}

static void test_rollback_failure(void) {
    clear_active_result = CUP_ERR_INCONSISTENT_STATE;
    rollback_move_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, command_remove("compiler", "clang@22.1.5", NULL));

    reset_scenario();
    initial_move_result = CUP_ERR_FILESYSTEM;
    transaction_clear_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ROLLBACK, command_remove("compiler", "clang@22.1.5", NULL));
}

static void test_state_commit_error(void) {
    state_save_result = CUP_ERR_COMMIT;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, move_calls);
    TEST_ASSERT_EQUAL_INT(0, transaction_clear_calls);

    reset_scenario();
    remove_tree_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);

    reset_scenario();
    transaction_clear_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);
}

static void test_wrapper_commit(void) {
    default_entry = "clang@22.1.5";
    plan_apply_result = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_COMMIT, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);
}

static void test_remove_success(void) {
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(0, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(0, plan_apply_calls);
    TEST_ASSERT_EQUAL_INT(1, remove_tree_calls);
    TEST_ASSERT_EQUAL_INT(1, transaction_clear_calls);

    reset_scenario();
    default_entry = "clang@22.1.5";
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_remove("compiler", "clang@22.1.5", NULL));
    TEST_ASSERT_EQUAL_INT(1, plan_build_calls);
    TEST_ASSERT_EQUAL_INT(1, plan_apply_calls);
}

static void test_inferred_remove_selection(void) {
    CupError result;
    char *output;
    const char *heading;
    const char *first_release;
    const char *second_release;
    const char *instruction;

    TEST_ASSERT_EQUAL_INT(CUP_OK, command_remove(NULL, "clang@22.1.5", NULL));

    reset_scenario();
    installed_match_count = 1;
    TEST_ASSERT_EQUAL_INT(CUP_OK, command_remove(NULL, "clang", NULL));

    reset_scenario();
    installed_match_count = 2;
    output = capture_ambiguous_remove(&result);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, result);
    heading = strstr(output, "Installed releases:\n");
    first_release = strstr(output, "  clang@21.1.5\n");
    second_release = strstr(output, "  clang@22.1.5\n");
    instruction = strstr(
        output,
        "Specify one of the installed releases with:\n"
        "  cup remove compiler clang@<release> --target linux-x64\n");
    TEST_ASSERT_NOT_NULL(heading);
    TEST_ASSERT_NOT_NULL(first_release);
    TEST_ASSERT_NOT_NULL(second_release);
    TEST_ASSERT_NOT_NULL(instruction);
    TEST_ASSERT_TRUE(heading < first_release);
    TEST_ASSERT_TRUE(first_release < second_release);
    TEST_ASSERT_TRUE(second_release < instruction);
    TEST_ASSERT_EQUAL_INT(0, move_calls);
    TEST_ASSERT_EQUAL_INT(0, remove_tree_calls);
    assert_common_cleanup();
    free(output);

    reset_scenario();
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, command_remove(NULL, "clang", NULL));
}

/* Suite registration. */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_remove_prepare_fail);
    RUN_TEST(test_staging_rollback);
    RUN_TEST(test_uncertain_staging);
    RUN_TEST(test_safe_rollback);
    RUN_TEST(test_rollback_failure);
    RUN_TEST(test_state_commit_error);
    RUN_TEST(test_wrapper_commit);
    RUN_TEST(test_remove_success);
    RUN_TEST(test_inferred_remove_selection);
    return UNITY_END();
}
