#ifndef CUP_REGISTRY_H
#define CUP_REGISTRY_H

#include "error.h"

CupError validate_component(const char *component);
CupError validate_tool_for_component(const char *component, const char *tool);

#endif /* CUP_REGISTRY_H */