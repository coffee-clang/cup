#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

#include "error.h"

CupError handle_list(const char *platform_override);
CupError handle_install(const char *component, const char *entry, const char *format_override, const char *platform_override);
CupError handle_remove(const char *component, const char *entry, const char *platform_override);
CupError handle_default(const char *component, const char *entry, const char *platform_override);
CupError handle_current(const char *component, const char *platform_override);

#endif /* CUP_COMMANDS_H */