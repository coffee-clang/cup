#ifndef CUP_REGISTRY_H
#define CUP_REGISTRY_H

#include <stddef.h>

#include "error.h"

/* Validate the built-in component and tool registry. */
CupError registry_validate_component(const char *component);
CupError registry_validate_tool(const char *component, const char *tool);
int registry_is_component(const char *component);
CupError registry_find_tool_component(const char *tool, char *component,
    size_t component_size);

#endif /* CUP_REGISTRY_H */
