#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

#include "error.h"

/* CLI command handlers. */
CupError command_list(const char *target_override);
CupError command_install(const char *component, const char *entry,
    const char *target_override, const char *format_override);
CupError command_remove(const char *component, const char *entry, const char *target_override);
CupError command_default(const char *component, const char *entry, const char *target_override);
CupError command_current(const char *component, const char *target_override);
CupError command_info(const char *component, const char *entry, const char *target_override);
CupError command_doctor(void);
CupError command_repair(void);
CupError command_uninstall(void);

#endif /* CUP_COMMANDS_H */
