#include "registry.h"

#include "text.h"

#include <stdio.h>
#include <string.h>

#define MAX_TOOLS_PER_COMPONENT 8

// COMPONENT REGISTRY
typedef struct {
    const char *component;
    const char *tools[MAX_TOOLS_PER_COMPONENT];
} SupportedComponent;

/*
 * Components and tools recognized by this cup build.
 * This registry validates the domain accepted by the CLI and state files;
 * concrete versions, formats and URLs are provided by the manifest.
 */
static const SupportedComponent SUPPORTED_COMPONENTS[] = {
    { "compiler", { "gcc", "clang", NULL } },
    { "debugger", { "gdb", "lldb", NULL } },
    { "linker", { "lld", NULL } },
    { "formatter", { "clang-format", NULL } },
    { "linter", { "clang-tidy", NULL } },
    { "language-server", { "clangd", NULL } },
    { "analyzer", { "valgrind", NULL } }
};

// REGISTRY LOOKUP
static const SupportedComponent *find_supported_component(const char *component) {
    size_t count;
    size_t i;

    if (text_is_empty(component)) {
        return NULL;
    }

    count = sizeof(SUPPORTED_COMPONENTS) / sizeof(SUPPORTED_COMPONENTS[0]);

    for (i = 0; i < count; ++i) {
        if (strcmp(SUPPORTED_COMPONENTS[i].component, component) == 0) {
            return &SUPPORTED_COMPONENTS[i];
        }
    }

    return NULL;
}

// PUBLIC API
CupError registry_validate_component(const char *component) {
    const SupportedComponent *supported;

    if (text_is_empty(component)) {
        return CUP_ERR_INVALID_INPUT;
    }

    supported = find_supported_component(component);
    if (supported != NULL) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported component '%s'.\n", component);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

CupError registry_validate_tool(const char *component, const char *tool) {
    const SupportedComponent *supported;
    size_t i;

    if (text_is_empty(component) || text_is_empty(tool)) {
        return CUP_ERR_INVALID_INPUT;
    }

    supported = find_supported_component(component);
    if (supported == NULL) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }

    for (i = 0; i < MAX_TOOLS_PER_COMPONENT && supported->tools[i] != NULL; ++i) {
        if (strcmp(supported->tools[i], tool) == 0) {
            return CUP_OK;
        }
    }

    fprintf(stderr, "Error: unsupported tool '%s' for component '%s'.\n", tool, component);
    return CUP_ERR_INVALID_TOOL;
}


int registry_is_component(const char *component) {
    return find_supported_component(component) != NULL;
}

CupError registry_find_tool_component(const char *tool, char *component,
    size_t component_size) {
    const SupportedComponent *matched = NULL;
    size_t count;
    size_t i;
    size_t j;

    if (text_is_empty(tool) || component == NULL || component_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    count = sizeof(SUPPORTED_COMPONENTS) / sizeof(SUPPORTED_COMPONENTS[0]);
    for (i = 0; i < count; ++i) {
        for (j = 0; j < MAX_TOOLS_PER_COMPONENT &&
            SUPPORTED_COMPONENTS[i].tools[j] != NULL; ++j) {
            if (strcmp(SUPPORTED_COMPONENTS[i].tools[j], tool) == 0) {
                if (matched != NULL && matched != &SUPPORTED_COMPONENTS[i]) {
                    fprintf(stderr,
                        "Error: tool '%s' belongs to more than one component.\n",
                        tool);
                    return CUP_ERR_INCONSISTENT_STATE;
                }
                matched = &SUPPORTED_COMPONENTS[i];
            }
        }
    }

    if (matched == NULL) {
        fprintf(stderr, "Error: unsupported tool or component '%s'.\n", tool);
        return CUP_ERR_INVALID_TOOL;
    }

    return text_format(component, component_size, "%s", matched->component);
}
