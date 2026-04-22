#include "support.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *component;
    const char *tools[8];
} SupportedComponent;

static const SupportedComponent SUPPORTED_COMPONENTS[] = {
    { "compiler", { "gcc", "clang", NULL } },
    { "debugger", { "gdb", NULL } }
};

static const SupportedComponent *find_supported_component(const char *component) {
    size_t i;

    if (component == NULL) {
        return NULL;
    }

    for (i = 0; i < sizeof(SUPPORTED_COMPONENTS) / sizeof(SUPPORTED_COMPONENTS[0]); ++i) {
        if (strcmp(SUPPORTED_COMPONENTS[i].component, component) == 0) {
            return &SUPPORTED_COMPONENTS[i];
        }
    }

    return NULL;
}

CupError validate_component(const char *component) {
    if (component == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (find_supported_component(component) != NULL) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported component '%s'.\n", component);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

CupError validate_tool_for_component(const char *component, const char *tool) {
    const SupportedComponent *supported;
    int i;

    if (component == NULL || tool == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    supported = find_supported_component(component);
    if (supported == NULL) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }

    for (i = 0; supported->tools[i] != NULL; ++i) {
        if (strcmp(supported->tools[i], tool) == 0) {
            return CUP_OK;
        }
    }

    fprintf(stderr, "Error: unsupported tool '%s' for component '%s'.\n", tool, component);
    return CUP_ERR_INVALID_TOOL;
}