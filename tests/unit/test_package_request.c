/*
 * Enforces command-level package selector validation and stable-to-concrete
 * resolution before package requests reach runtime consumers.
 */

#include "package_request.h"
#include "domain_registry.h"
#include "package_catalog.h"
#include "path.h"
#include "registry.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static CupError stable_result;
static char stable_release[MAX_IDENTIFIER_LEN];

typedef struct {
    const char *component;
    const char *tool;
} RegisteredTool;

#define TEST_COMPONENT_ENTRY(name) name,
static const char *const REGISTERED_COMPONENTS[] = {
    CUP_COMPONENT_REGISTRY(TEST_COMPONENT_ENTRY)
};
#undef TEST_COMPONENT_ENTRY

#define TEST_TOOL_ENTRY(component, tool) {component, tool},
static const RegisteredTool REGISTERED_TOOLS[] = {
    CUP_TOOL_REGISTRY(TEST_TOOL_ENTRY)
};
#undef TEST_TOOL_ENTRY

#define TEST_PLATFORM_ENTRY(os, arch) os "-" arch,
static const char *const REGISTERED_PLATFORMS[] = {
    CUP_PLATFORM_REGISTRY(TEST_PLATFORM_ENTRY)
};
#undef TEST_PLATFORM_ENTRY

static int is_lower_ascii(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;

    while (*cursor != '\0') {
        if (*cursor >= 'A' && *cursor <= 'Z') {
            return 0;
        }
        cursor++;
    }
    return 1;
}

void setUp(void) {
    stable_result = CUP_OK;
    strcpy(stable_release, "22.1.5");
}

void tearDown(void) {
}

static void test_registry_contract(void) {
    char component[MAX_IDENTIFIER_LEN];
    size_t i;
    size_t j;

    TEST_ASSERT_EQUAL_size_t(sizeof(REGISTERED_COMPONENTS) / sizeof(REGISTERED_COMPONENTS[0]),
                             registry_component_count());
    TEST_ASSERT_EQUAL_size_t(sizeof(REGISTERED_TOOLS) / sizeof(REGISTERED_TOOLS[0]),
                             CUP_TOOL_COUNT);
    TEST_ASSERT_EQUAL_size_t(CUP_COMPONENT_COUNT, registry_component_count());
    TEST_ASSERT_EQUAL_size_t(CUP_PLATFORM_COUNT,
                             sizeof(REGISTERED_PLATFORMS) / sizeof(REGISTERED_PLATFORMS[0]));
    TEST_ASSERT_EQUAL_size_t(CUP_GLOBAL_SCOPE_COUNT,
                             CUP_COMPONENT_COUNT * CUP_PLATFORM_COUNT * CUP_PLATFORM_COUNT);

    for (i = 0; i < registry_component_count(); ++i) {
        TEST_ASSERT_TRUE(strlen(REGISTERED_COMPONENTS[i]) < MAX_IDENTIFIER_LEN);
        TEST_ASSERT_TRUE(path_is_safe_identifier(REGISTERED_COMPONENTS[i]));
        TEST_ASSERT_TRUE(is_lower_ascii(REGISTERED_COMPONENTS[i]));
        TEST_ASSERT_EQUAL_STRING(REGISTERED_COMPONENTS[i], registry_component_at(i));
        TEST_ASSERT_TRUE(registry_is_component(REGISTERED_COMPONENTS[i]));
        TEST_ASSERT_EQUAL_INT(CUP_OK, registry_validate_component(REGISTERED_COMPONENTS[i]));
        for (j = i + 1; j < registry_component_count(); ++j) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(REGISTERED_COMPONENTS[i], REGISTERED_COMPONENTS[j]));
        }
    }
    TEST_ASSERT_NULL(registry_component_at(registry_component_count()));

    for (i = 0; i < CUP_TOOL_COUNT; ++i) {
        TEST_ASSERT_TRUE(strlen(REGISTERED_TOOLS[i].component) < MAX_IDENTIFIER_LEN);
        TEST_ASSERT_TRUE(strlen(REGISTERED_TOOLS[i].tool) < MAX_IDENTIFIER_LEN);
        TEST_ASSERT_TRUE(path_is_safe_identifier(REGISTERED_TOOLS[i].component));
        TEST_ASSERT_TRUE(path_is_safe_identifier(REGISTERED_TOOLS[i].tool));
        TEST_ASSERT_TRUE(is_lower_ascii(REGISTERED_TOOLS[i].component));
        TEST_ASSERT_TRUE(is_lower_ascii(REGISTERED_TOOLS[i].tool));
        TEST_ASSERT_TRUE(registry_is_component(REGISTERED_TOOLS[i].component));
        TEST_ASSERT_TRUE(
            registry_is_tool(REGISTERED_TOOLS[i].component, REGISTERED_TOOLS[i].tool));
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              registry_validate_tool(REGISTERED_TOOLS[i].component,
                                                     REGISTERED_TOOLS[i].tool));
        TEST_ASSERT_EQUAL_INT(CUP_OK,
                              registry_find_tool_component(
                                  REGISTERED_TOOLS[i].tool, component, sizeof(component)));
        TEST_ASSERT_EQUAL_STRING(REGISTERED_TOOLS[i].component, component);
        for (j = i + 1; j < CUP_TOOL_COUNT; ++j) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(REGISTERED_TOOLS[i].tool, REGISTERED_TOOLS[j].tool));
        }
    }

    for (i = 0; i < sizeof(REGISTERED_PLATFORMS) / sizeof(REGISTERED_PLATFORMS[0]); ++i) {
        TEST_ASSERT_TRUE(strlen(REGISTERED_PLATFORMS[i]) < MAX_PLATFORM_LEN);
        TEST_ASSERT_TRUE(path_is_safe_identifier(REGISTERED_PLATFORMS[i]));
        TEST_ASSERT_TRUE(is_lower_ascii(REGISTERED_PLATFORMS[i]));
        for (j = i + 1; j < sizeof(REGISTERED_PLATFORMS) / sizeof(REGISTERED_PLATFORMS[0]); ++j) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(REGISTERED_PLATFORMS[i], REGISTERED_PLATFORMS[j]));
        }
    }

    TEST_ASSERT_FALSE(registry_is_component(NULL));
    TEST_ASSERT_FALSE(registry_is_tool(NULL, "clang"));
    TEST_ASSERT_FALSE(registry_is_tool("compiler", NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_component(NULL));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT, registry_validate_tool(NULL, "clang"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT, registry_validate_component("unknown"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL, registry_validate_tool("compiler", "gdb"));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          registry_find_tool_component("unknown", component, sizeof(component)));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          registry_find_tool_component("clang", component, 2));
}

CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *release,
                                        size_t release_size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform) {
    (void)catalog;
    (void)component;
    (void)tool;
    (void)host_platform;
    (void)target_platform;
    if (stable_result != CUP_OK) {
        return stable_result;
    }
    if (strlen(stable_release) + 1 > release_size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    strcpy(release, stable_release);
    return CUP_OK;
}

static void test_parse_request(void) {
    PackageRequest request;
    char long_selector[MAX_SELECTOR_LEN + 1];

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_STRING("clang", request.selector.tool);
    TEST_ASSERT_EQUAL_STRING("stable", request.selector.release);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@22.1.5-rev1", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_request_parse("compiler", "clang@22.1.5-RC1", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_request_parse("compiler", "clang@STABLE", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          package_request_parse("compiler", "CLANG@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_TOOL,
                          package_request_parse("compiler", "gdb@22.1.5", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_UNSUPPORTED_COMPONENT,
                          package_request_parse("unknown", "clang@22.1.5", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse("compiler", "clang", &request));
    TEST_ASSERT_EQUAL_STRING("", request.input_selector);
    TEST_ASSERT_EQUAL_STRING("", request.selector.tool);
    TEST_ASSERT_EQUAL_STRING("", request.selector.release);
    memset(long_selector, 'x', sizeof(long_selector) - 1);
    long_selector[sizeof(long_selector) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(CUP_ERR_BUFFER_TOO_SMALL,
                          package_request_parse("compiler", long_selector, &request));
    TEST_ASSERT_EQUAL_STRING("", request.input_selector);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse("", "clang@22.1.5", &request));
    TEST_ASSERT_EQUAL_STRING("", request.input_selector);
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_parse("compiler", "clang@22.1.5", NULL));
}

static void test_resolve_request(void) {
    PackageCatalog catalog = {0};
    PackageRequest request;

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_resolve(&catalog,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    TEST_ASSERT_EQUAL_STRING("22.1.5", request.resolved_release);
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", request.resolved_selector);

    stable_result = CUP_ERR_NOT_AVAILABLE;
    TEST_ASSERT_EQUAL_INT(CUP_ERR_NOT_AVAILABLE,
                          package_request_resolve(&catalog,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    TEST_ASSERT_EQUAL_STRING("", request.resolved_release);
    TEST_ASSERT_EQUAL_STRING("", request.resolved_selector);
    stable_result = CUP_OK;

    strcpy(stable_release, "22.1.5-RC1");
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_RELEASE,
                          package_request_resolve(&catalog,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@22.1.5", &request));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_resolve(NULL,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    TEST_ASSERT_EQUAL_STRING("22.1.5", request.resolved_release);
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", request.resolved_selector);

    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_CATALOG,
                          package_request_resolve(NULL,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    TEST_ASSERT_EQUAL_INT(CUP_ERR_INVALID_INPUT,
                          package_request_resolve(NULL,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  NULL));
}

static void test_print_request(void) {
    PackageRequest request;
    PackageCatalog catalog = {0};
    FILE *stream = tmpfile();
    char output[128] = {0};
    size_t count;

    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@stable", &request));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_resolve(&catalog,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    package_request_print(stream, &request);
    TEST_ASSERT_EQUAL_INT(0, fflush(stream));
    TEST_ASSERT_EQUAL_INT(0, fseek(stream, 0, SEEK_SET));
    count = fread(output, 1, sizeof(output) - 1, stream);
    output[count] = '\0';
    TEST_ASSERT_EQUAL_STRING("clang@stable -> clang@22.1.5", output);
    TEST_ASSERT_EQUAL_INT(0, fclose(stream));

    stream = tmpfile();
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_parse("compiler", "clang@22.1.5", &request));
    TEST_ASSERT_EQUAL_INT(CUP_OK,
                          package_request_resolve(NULL,
                                                  "compiler",
                                                  "linux-x64",
                                                  "linux-x64",
                                                  &request));
    memset(output, 0, sizeof(output));
    package_request_print(stream, &request);
    TEST_ASSERT_EQUAL_INT(0, fflush(stream));
    TEST_ASSERT_EQUAL_INT(0, fseek(stream, 0, SEEK_SET));
    count = fread(output, 1, sizeof(output) - 1, stream);
    output[count] = '\0';
    TEST_ASSERT_EQUAL_STRING("clang@22.1.5", output);
    TEST_ASSERT_EQUAL_INT(0, fclose(stream));

    package_request_print(NULL, &request);
    package_request_print(stdout, NULL);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_registry_contract);
    RUN_TEST(test_parse_request);
    RUN_TEST(test_resolve_request);
    RUN_TEST(test_print_request);
    return UNITY_END();
}
