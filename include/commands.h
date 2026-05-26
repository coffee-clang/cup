#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

#include "error.h"

/*
 * Command handlers used by the CLI dispatcher.
 *
 * Entries must use the '<tool>@<release>' syntax.
 * target_override may be NULL or empty to use the detected host platform.
 * format_override may be NULL or empty to use the package default format.
 */
CupError handle_list(const char *target_override);
CupError handle_install(const char *component, const char *entry, const char *target_override, const char *format_override);
CupError handle_remove(const char *component, const char *entry, const char *target_override);
CupError handle_default(const char *component, const char *entry, const char *target_override);
CupError handle_current(const char *component, const char *target_override);
CupError handle_info(const char *component, const char *entry, const char *target_override);
CupError handle_doctor(void);
CupError handle_repair(void);
CupError handle_uninstall(void);

#endif /* CUP_COMMANDS_H */