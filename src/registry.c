#include "registry.h"

#include <stdio.h>
#include <string.h>

#define MAX_TOOLS_PER_COMPONENT 8

typedef struct {
    const char *component;
    const char *tools[MAX_TOOLS_PER_COMPONENT];
} SupportedComponent;

static const SupportedComponent SUPPORTED_COMPONENTS[] = {
    { "compiler", { "gcc", "clang", NULL } },
    { "debugger", { "gdb", "lldb", NULL } }
};

static const SupportedComponent *find_supported_component(const char *component) {
    size_t count;
    size_t i;

    if (component == NULL || component[0] == '\0') {
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

CupError validate_component(const char *component) {
    const SupportedComponent *supported;

    if (component == NULL || component[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    supported = find_supported_component(component);
    if (supported != NULL) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported component '%s'.\n", component);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

CupError validate_tool_for_component(const char *component, const char *tool) {
    const SupportedComponent *supported;
    size_t i;

    if (component == NULL || tool == NULL ||
        component[0] == '\0' || tool[0] == '\0') {
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