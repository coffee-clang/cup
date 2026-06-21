#ifndef CUP_REGISTRY_H
#define CUP_REGISTRY_H

#include "error.h"

/* Validate the built-in component and tool registry. */
CupError registry_validate_component(const char *component);
CupError registry_validate_tool(const char *component, const char *tool);

#endif /* CUP_REGISTRY_H */
