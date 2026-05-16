#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

#include "error.h"

CupError handle_list(const char *target_override);
CupError handle_install(const char *component, const char *entry, const char *target_override, const char *format_override);
CupError handle_remove(const char *component, const char *entry, const char *target_override);
CupError handle_default(const char *component, const char *entry, const char *target_override);
CupError handle_current(const char *component, const char *target_override);
CupError handle_doctor(void);
CupError handle_repair(void);
CupError handle_uninstall(void);

#endif /* CUP_COMMANDS_H */