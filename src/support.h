#ifndef SUPPORT_H
#define SUPPORT_H

#include "error.h"

CupError validate_component(const char *component);
CupError validate_tool_for_component(const char *component, const char *tool);

#endif