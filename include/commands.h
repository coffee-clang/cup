#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

#include "error.h"

/* CLI command handlers. */
CupError command_list(const char *component, const char *target_override);
CupError command_install(const char *component, const char *entry,
    const char *target_override, const char *format_override);
CupError command_remove(const char *component, const char *entry, const char *target_override);
CupError command_default(const char *component, const char *entry, const char *target_override);
CupError command_search(const char *component, const char *target_override);
CupError command_info(const char *component, const char *target_override);
CupError command_inspect(const char *component, const char *entry,
    const char *target_override);
CupError command_update(const char *selector);
CupError command_self_update(void);
CupError command_doctor(void);
CupError command_repair(void);
CupError command_uninstall(void);

/* Internal command operation: ensure a tool's stable release is installed
 * in one scope and move its previous default with compare-and-swap semantics. */
CupError command_update_scope(const char *component, const char *tool,
    const char *target_override, const char *expected_default,
    int *installed, int *default_moved);

#endif /* CUP_COMMANDS_H */
