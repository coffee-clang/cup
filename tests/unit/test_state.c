/*
 * Test focus: Exercises structured state parsing, semantic validation, scope mutation, capacity
 * limits and atomic persistence.
 */

#include "error.h"
#include "package.h"
#include "state.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[] = "/tmp/cup-state-unit-test-XXXXXX";
static CupError state_path_error;

void setUp(void) {
    state_path_error = CUP_OK;
}
void tearDown(void) {
}

CupError layout_get_root(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return snprintf(buffer, size, "%s", temp_dir) >= 0 && strlen(temp_dir) < size
               ? CUP_OK
               : CUP_ERR_BUFFER_TOO_SMALL;
}

CupError layout_get_state_path(char *buffer, size_t size) {
    int written;

    if (state_path_error != CUP_OK) {
        return state_path_error;
    }
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    written = snprintf(buffer, size, "%s/state.txt", temp_dir);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static CupError copy_field(char *buffer, size_t size, const char *value) {
    size_t length;

    if (buffer == NULL || size == 0 || value == NULL || value[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }
    length = strlen(value);
    if (length >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, value, length + 1);
    return CUP_OK;
}

CupError package_scope_init(PackageScope *scope,
                            const char *component,
                            const char *host_platform,
                            const char *target_platform) {
    CupError err;

    if (scope == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(scope, 0, sizeof(*scope));
    err = copy_field(scope->component, sizeof(scope->component), component);
    if (err != CUP_OK) {
        return err;
    }
    err = copy_field(scope->host_platform, sizeof(scope->host_platform), host_platform);
    if (err != CUP_OK) {
        return err;
    }
    return copy_field(scope->target_platform, sizeof(scope->target_platform), target_platform);
}

int package_scope_equals(const PackageScope *left, const PackageScope *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0;
}

CupError package_identity_init(PackageIdentity *identity,
                               const char *component,
                               const char *tool,
                               const char *host_platform,
                               const char *target_platform,
                               const char *version) {
    PackageScope scope;
    CupError err;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = package_scope_init(&scope, component, host_platform, target_platform);
    if (err != CUP_OK) {
        return err;
    }
    if (tool == NULL || version == NULL || strcmp(version, "stable") == 0 ||
        strstr(version, "bad") != NULL) {
        return CUP_ERR_VALIDATION;
    }

    memset(identity, 0, sizeof(*identity));
    identity->component[0] = '\0';
    err = copy_field(identity->component, sizeof(identity->component), scope.component);
    if (err != CUP_OK) {
        return err;
    }
    err = copy_field(identity->tool, sizeof(identity->tool), tool);
    if (err != CUP_OK) {
        return err;
    }
    err = copy_field(identity->host_platform, sizeof(identity->host_platform), scope.host_platform);
    if (err != CUP_OK) {
        return err;
    }
    err = copy_field(
        identity->target_platform, sizeof(identity->target_platform), scope.target_platform);
    if (err != CUP_OK) {
        return err;
    }
    return copy_field(identity->version, sizeof(identity->version), version);
}

CupError package_identity_validate(const PackageIdentity *identity) {
    PackageIdentity validated;

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_identity_init(&validated,
                                 identity->component,
                                 identity->tool,
                                 identity->host_platform,
                                 identity->target_platform,
                                 identity->version);
}

CupError package_identity_from_selector(PackageIdentity *identity,
                                        const char *component,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *entry) {
    const char *at;
    char tool[MAX_IDENTIFIER_LEN];
    size_t tool_length;

    if (entry == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    at = strchr(entry, '@');
    if (at == NULL || at == entry || at[1] == '\0' || strchr(at + 1, '@') != NULL) {
        return CUP_ERR_VALIDATION;
    }
    tool_length = (size_t)(at - entry);
    if (tool_length >= sizeof(tool)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(tool, entry, tool_length);
    tool[tool_length] = '\0';
    return package_identity_init(identity, component, tool, host_platform, target_platform, at + 1);
}

CupError package_identity_get_scope(const PackageIdentity *identity, PackageScope *scope) {
    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_scope_init(
        scope, identity->component, identity->host_platform, identity->target_platform);
}

int package_identity_equals(const PackageIdentity *left, const PackageIdentity *right) {
    return left != NULL && right != NULL && strcmp(left->component, right->component) == 0 &&
           strcmp(left->tool, right->tool) == 0 &&
           strcmp(left->host_platform, right->host_platform) == 0 &&
           strcmp(left->target_platform, right->target_platform) == 0 &&
           strcmp(left->version, right->version) == 0;
}

CupError package_identity_format_selector(const PackageIdentity *identity,
                                          char *buffer,
                                          size_t size) {
    int written;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (package_identity_validate(identity) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }
    written = snprintf(buffer, size, "%s@%s", identity->tool, identity->version);
    return written >= 0 && (size_t)written < size ? CUP_OK : CUP_ERR_BUFFER_TOO_SMALL;
}

static PackageIdentity identity(const char *component,
                                const char *tool,
                                const char *target,
                                const char *version) {
    PackageIdentity value;

    TEST_ASSERT_EQUAL_INT(
        CUP_OK, package_identity_init(&value, component, tool, "linux-x64", target, version));
    return value;
}

static PackageScope scope(const char *component, const char *target) {
    PackageScope value;

    TEST_ASSERT_EQUAL_INT(CUP_OK, package_scope_init(&value, component, "linux-x64", target));
    return value;
}

static void state_path(char *path, size_t size) {
    int written = snprintf(path, size, "%s/state.txt", temp_dir);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size);
}

static void write_bytes(const void *data, size_t size) {
    char path[1024];
    FILE *file;

    state_path(path, sizeof(path));
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_raw_state(const char *text) {
    write_bytes(text, strlen(text));
}

static void write_state(const char *text) {
    char buffer[4096];
    int written = snprintf(buffer, sizeof(buffer), "format=1\n%s", text);

    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof(buffer));
    write_bytes(buffer, (size_t)written);
}

static char *read_state(void) {
    char path[1024];
    char *text;
    long length;
    FILE *file;

    state_path(path, sizeof(path));
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    length = ftell(file);
    TEST_ASSERT_TRUE(length >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    text = calloc((size_t)length + 1, 1);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_size_t((size_t)length, fread(text, 1, (size_t)length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    return text;
}

static void test_mutators(void) {
    CupState state = {0};
    PackageIdentity clang1 = identity("compiler", "clang", "linux-x64", "1.0.0");
    PackageIdentity clang2 = identity("compiler", "clang", "linux-x64", "2.0.0");
    PackageScope compiler = scope("compiler", "linux-x64");
    const PackageIdentity *current;

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ALREADY_INSTALLED, state_add_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(0, state_find_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(-1, state_find_installed(&state, &clang2));

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang1));
    current = state_get_active(&state, &compiler);
    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_TRUE(package_identity_equals(&clang1, current));

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang2));
    current = state_get_active(&state, &compiler);
    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_TRUE(package_identity_equals(&clang2, current));

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_clear_matching_active(&state, &clang1));
    TEST_ASSERT_NOT_NULL(state_get_active(&state, &compiler));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_clear_matching_active(&state, &clang2));
    TEST_ASSERT_NULL(state_get_active(&state, &compiler));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_clear_active(&state, &compiler));

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_remove_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_INSTALLED, state_remove_installed(&state, &clang1));
}

static void test_scope_mutation(void) {
    CupState state = {0};
    PackageIdentity clang_linux = identity("compiler", "clang", "linux-x64", "1");
    PackageIdentity clang_windows = identity("compiler", "clang", "windows-x64", "2");
    PackageIdentity gdb = identity("debugger", "gdb", "linux-x64", "1");
    PackageScope compiler = scope("compiler", "linux-x64");

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang_linux));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang_windows));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &gdb));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_remove_installed(&state, &clang_windows));
    TEST_ASSERT_EQUAL_size_t(2, state.installed_count);
    TEST_ASSERT_EQUAL_STRING("gdb", state.installed[1].tool);
    TEST_ASSERT_EQUAL_STRING("1", state.installed[1].version);

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang_linux));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &gdb));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_clear_active(&state, &compiler));
    TEST_ASSERT_EQUAL_size_t(1, state.active_count);
    TEST_ASSERT_EQUAL_STRING("debugger", state.active[0].component);
}

static void test_mutator_guards(void) {
    CupState state = {0};
    PackageIdentity valid = identity("compiler", "clang", "linux-x64", "1");
    PackageIdentity invalid = {0};
    PackageScope valid_scope = scope("compiler", "linux-x64");
    PackageScope invalid_scope = {0};

    TEST_ASSERT_EQUAL_INT(-1, state_find_installed(NULL, &valid));
    TEST_ASSERT_EQUAL_INT(-1, state_find_installed(&state, NULL));
    TEST_ASSERT_EQUAL_INT(-1, state_find_installed(&state, &invalid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_add_installed(NULL, &valid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_add_installed(&state, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_remove_installed(NULL, &valid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_remove_installed(&state, &invalid));
    TEST_ASSERT_EQUAL_INT(-1, state_find_active(NULL, &valid_scope));
    TEST_ASSERT_EQUAL_INT(-1, state_find_active(&state, NULL));
    TEST_ASSERT_EQUAL_INT(-1, state_find_active(&state, &invalid_scope));
    TEST_ASSERT_NULL(state_get_active(NULL, &valid_scope));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_set_active(NULL, &valid));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_set_active(&state, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_clear_active(NULL, &valid_scope));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_clear_matching_active(NULL, &valid));
}

static void test_capacity_limits(void) {
    CupState state = {0};
    PackageIdentity valid = identity("compiler", "clang", "linux-x64", "1");
    PackageIdentity invalid = valid;

    state.installed_count = MAX_INSTALLED;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_FULL, state_add_installed(&state, &valid));

    memset(&state, 0, sizeof(state));
    state.active_count = MAX_ACTIVE_PACKAGES;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_ACTIVE_FULL, state_set_active(&state, &valid));

    memset(invalid.component, 'x', sizeof(invalid.component));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, state_add_installed(&(CupState){0}, &invalid));

    invalid = valid;
    memset(invalid.host_platform, 'p', sizeof(invalid.host_platform));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL, state_set_active(&(CupState){0}, &invalid));
}

static void test_validation(void) {
    CupState state = {0};
    PackageIdentity clang1 = identity("compiler", "clang", "linux-x64", "1");
    PackageIdentity clang2 = identity("compiler", "clang", "linux-x64", "2");

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_validate(NULL));
    state.installed_count = MAX_INSTALLED + 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));
    memset(&state, 0, sizeof(state));
    state.active_count = MAX_ACTIVE_PACKAGES + 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));

    memset(&state, 0, sizeof(state));
    state.installed[0] = clang1;
    state.installed[1] = clang1;
    state.installed_count = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));

    memset(&state, 0, sizeof(state));
    state.installed[0] = clang1;
    state.installed[0].version[0] = '\0';
    state.installed_count = 1;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));

    memset(&state, 0, sizeof(state));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang2));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));

    memset(&state, 0, sizeof(state));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang1));
    state.active[1] = state.active[0];
    state.active_count = 2;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));

    memset(&state, 0, sizeof(state));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang1));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang1));
    state.active[0].version[0] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_validate(&state));
}

static void test_save_load(void) {
    CupState state = {0};
    CupState loaded;
    StateFileStatus status;
    PackageIdentity clang = identity("compiler", "clang", "linux-x64", "22.1.5");
    PackageIdentity lldb = identity("debugger", "lldb", "linux-x64", "22.1.5");
    char path[1024];
    char *serialized;

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &clang));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &lldb));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_set_active(&state, &clang));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_save(&state));

    serialized = read_state();
    TEST_ASSERT_EQUAL_STRING("format=1\n"
                             "installed.compiler.linux-x64.linux-x64=clang@22.1.5\n"
                             "installed.debugger.linux-x64.linux-x64=lldb@22.1.5\n"
                             "default.compiler.linux-x64.linux-x64=clang@22.1.5\n",
                             serialized);
    free(serialized);

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_LOADED, status);
    TEST_ASSERT_EQUAL_size_t(2, loaded.installed_count);
    TEST_ASSERT_EQUAL_size_t(1, loaded.active_count);
    TEST_ASSERT_EQUAL_STRING("clang", loaded.installed[0].tool);
    TEST_ASSERT_EQUAL_STRING("22.1.5", loaded.installed[0].version);
    TEST_ASSERT_TRUE(package_identity_equals(&clang, &loaded.active[0]));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_validate(&loaded));

    state_path(path, sizeof(path));
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_MISSING, status);
    TEST_ASSERT_EQUAL_size_t(0, loaded.installed_count);

    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_load(NULL, &status));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_load(&loaded, NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_save(NULL));
    state_path_error = CUP_ERR_FILESYSTEM;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_FILESYSTEM, state_load(&loaded, &status));
    state_path_error = CUP_OK;

    state_path(path, sizeof(path));
    TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0755));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&loaded, &status));
    TEST_ASSERT_EQUAL_INT(0, rmdir(path));
}

static void test_state_record_errors(void) {
    CupState state;
    StateFileStatus status;

    write_state("unknown.key=value\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
    write_state("installed.compiler.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
    write_state("default.compiler.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
    write_state("installed.compiler.linux-x64.linux-x64\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
    write_state("installed.compiler.linux-x64.linux-x64=bad-entry\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));

    write_state("installed.compiler.linux-x64.linux-x64=clang@1\n"
                "installed.compiler.linux-x64.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_MISSING, status);

    write_state("installed.compiler.linux-x64.linux-x64=clang@1\n"
                "default.compiler.linux-x64.linux-x64=clang@1\n"
                "default.compiler.linux-x64.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));
}

static void test_state_line_errors(void) {
    CupState state;
    StateFileStatus status;
    unsigned char bad[] = {'i', 'n', 's', 't', 1, '\n'};
    char long_line[MAX_STATE_LINE_LEN + 16];

    {
        unsigned char prefixed[sizeof(bad) + 9];
        memcpy(prefixed, "format=1\n", 9);
        memcpy(prefixed + 9, bad, sizeof(bad));
        write_bytes(prefixed, sizeof(prefixed));
    }
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));

    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\n';
    {
        char prefixed[sizeof(long_line) + 9];
        memcpy(prefixed, "format=1\n", 9);
        memcpy(prefixed + 9, long_line, sizeof(long_line));
        write_bytes(prefixed, sizeof(prefixed));
    }
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&state, &status));

    write_state("# comment\n\n"
                "installed.compiler.linux-x64.linux-x64=clang@1\r\n");
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_load(&state, &status));
    TEST_ASSERT_EQUAL_INT(STATE_FILE_LOADED, status);
    TEST_ASSERT_EQUAL_size_t(1, state.installed_count);
    TEST_ASSERT_EQUAL_STRING("clang", state.installed[0].tool);
    TEST_ASSERT_EQUAL_STRING("1", state.installed[0].version);
}

static void test_format_host_policy(void) {
    CupState state = {0};
    CupState loaded;
    StateFileStatus status;
    PackageIdentity native = identity("compiler", "clang", "linux-x64", "1");
    PackageIdentity foreign = native;

    write_raw_state("installed.compiler.linux-x64.linux-x64=clang@1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&loaded, &status));
    write_raw_state("format=2\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&loaded, &status));
    write_state("format=1\n");
    TEST_ASSERT_EQUAL_INT(CUP_ERR_STATE_LOAD, state_load(&loaded, &status));

    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &native));
    TEST_ASSERT_EQUAL_size_t(0, state_count_foreign_hosts(&state, "linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_validate_current_host(&state, "linux-x64"));

    TEST_ASSERT_EQUAL_INT(
        CUP_OK,
        package_identity_init(&foreign, "compiler", "clang", "windows-x64", "windows-x64", "1"));
    TEST_ASSERT_EQUAL_INT(CUP_OK, state_add_installed(&state, &foreign));
    TEST_ASSERT_EQUAL_size_t(1, state_count_foreign_hosts(&state, "linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INCONSISTENT_STATE,
                          state_validate_current_host(&state, "linux-x64"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, state_validate_current_host(NULL, "linux-x64"));
}

int main(void) {
    TEST_ASSERT_NOT_NULL(mkdtemp(temp_dir));
    UNITY_BEGIN();
    RUN_TEST(test_mutators);
    RUN_TEST(test_scope_mutation);
    RUN_TEST(test_mutator_guards);
    RUN_TEST(test_capacity_limits);
    RUN_TEST(test_validation);
    RUN_TEST(test_save_load);
    RUN_TEST(test_state_record_errors);
    RUN_TEST(test_state_line_errors);
    RUN_TEST(test_format_host_policy);
    return UNITY_END();
}
