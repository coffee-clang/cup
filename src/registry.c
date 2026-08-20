/*
 * Defines the supported component/tool relationships compiled into cup. The catalog can restrict
 * availability but cannot extend this domain.
 */

#include "registry.h"

#include "constants.h"
#include "domain_registry.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *component;
    const char *tool;
} SupportedTool;

#define ASSERT_COMPONENT_CAPACITY(name) \
    _Static_assert(sizeof(name) <= MAX_IDENTIFIER_LEN, "component exceeds identifier capacity");
CUP_COMPONENT_REGISTRY(ASSERT_COMPONENT_CAPACITY)
#undef ASSERT_COMPONENT_CAPACITY

#define ASSERT_TOOL_CAPACITY(component, tool)                                              \
    _Static_assert(sizeof(component) <= MAX_IDENTIFIER_LEN,                               \
                   "tool component exceeds identifier capacity");                        \
    _Static_assert(sizeof(tool) <= MAX_IDENTIFIER_LEN, "tool exceeds identifier capacity");
CUP_TOOL_REGISTRY(ASSERT_TOOL_CAPACITY)
#undef ASSERT_TOOL_CAPACITY

#define COMPONENT_ENTRY(name) name,
static const char *const SUPPORTED_COMPONENTS[] = {
    CUP_COMPONENT_REGISTRY(COMPONENT_ENTRY)
};
#undef COMPONENT_ENTRY

#define TOOL_ENTRY(component, tool) {component, tool},
static const SupportedTool SUPPORTED_TOOLS[] = {
    CUP_TOOL_REGISTRY(TOOL_ENTRY)
};
#undef TOOL_ENTRY

_Static_assert(sizeof(SUPPORTED_COMPONENTS) / sizeof(SUPPORTED_COMPONENTS[0]) ==
                   CUP_COMPONENT_COUNT,
               "component registry count must remain derived");
_Static_assert(sizeof(SUPPORTED_TOOLS) / sizeof(SUPPORTED_TOOLS[0]) == CUP_TOOL_COUNT,
               "tool registry count must remain derived");

static int find_component_index(const char *component) {
    size_t i;

    if (text_is_empty(component)) {
        return -1;
    }
    for (i = 0; i < registry_component_count(); ++i) {
        if (strcmp(SUPPORTED_COMPONENTS[i], component) == 0) {
            return (int)i;
        }
    }
    return -1;
}

CupError registry_validate_component(const char *component) {
    if (text_is_empty(component)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (find_component_index(component) >= 0) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported component '%s'.\n", component);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

CupError registry_validate_tool(const char *component, const char *tool) {
    if (text_is_empty(component) || text_is_empty(tool)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (find_component_index(component) < 0) {
        fprintf(stderr, "Error: unsupported component '%s'.\n", component);
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }

    if (registry_is_tool(component, tool)) {
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported tool '%s' for component '%s'.\n", tool, component);
    return CUP_ERR_INVALID_TOOL;
}

int registry_is_component(const char *component) {
    return find_component_index(component) >= 0;
}

int registry_is_tool(const char *component, const char *tool) {
    size_t i;

    if (text_is_empty(component) || text_is_empty(tool) || find_component_index(component) < 0) {
        return 0;
    }
    for (i = 0; i < CUP_TOOL_COUNT; ++i) {
        if (strcmp(SUPPORTED_TOOLS[i].component, component) == 0 &&
            strcmp(SUPPORTED_TOOLS[i].tool, tool) == 0) {
            return 1;
        }
    }
    return 0;
}

size_t registry_component_count(void) {
    return CUP_COMPONENT_COUNT;
}

const char *registry_component_at(size_t index) {
    return index < registry_component_count() ? SUPPORTED_COMPONENTS[index] : NULL;
}

CupError registry_find_tool_component(const char *tool, char *component, size_t component_size) {
    const char *matched = NULL;
    size_t i;

    if (text_is_empty(tool) || component == NULL || component_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < CUP_TOOL_COUNT; ++i) {
        if (strcmp(SUPPORTED_TOOLS[i].tool, tool) == 0) {
            if (matched != NULL && strcmp(matched, SUPPORTED_TOOLS[i].component) != 0) {
                fprintf(stderr, "Error: tool '%s' belongs to more than one component.\n", tool);
                return CUP_ERR_INCONSISTENT_STATE;
            }
            matched = SUPPORTED_TOOLS[i].component;
        }
    }

    if (matched == NULL) {
        fprintf(stderr, "Error: unsupported tool '%s'.\n", tool);
        return CUP_ERR_INVALID_TOOL;
    }
    return text_copy(component, component_size, matched);
}
