#ifndef CUP_REGISTRY_H
#define CUP_REGISTRY_H

/*
 * Compiled component/tool domain accepted independently of catalog availability. The catalog may
 * restrict packages but cannot add component/tool relationships.
 */

#include <stddef.h>

#include "error.h"

/* Validate a component, or one tool within its registered component. */
CupError registry_validate_component(const char *component);
CupError registry_validate_tool(const char *component, const char *tool);

/* Return whether a selector is the exact name of a registered component. */
int registry_is_component(const char *component);
size_t registry_component_count(void);
const char *registry_component_at(size_t index);
size_t registry_tool_count(const char *component);
const char *registry_tool_at(const char *component, size_t index);

/* Resolve the unique registered component that owns a tool name. */
CupError registry_find_tool_component(const char *tool, char *component, size_t component_size);

#endif /* CUP_REGISTRY_H */
