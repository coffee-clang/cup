#ifndef COMPONENT_H
#define COMPONENT_H

#include "error.h"

CupError handle_list(void);
CupError handle_install(const char *component, const char *entry, const char *format_override);
CupError handle_remove(const char *component, const char *entry);
CupError handle_default(const char *component, const char *entry);
CupError handle_current(const char *component);

#endif