#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

/*
 * Module contract: Public CLI command handlers. Handlers receive already
 * parsed arguments and
 * own command-level validation, locking, output, and state transitions.
 */

#include "error.h"

/* List installed packages, optionally restricted by component or target. */
CupError command_list(const char *component, const char *target_override);

/* Install one concrete or stable-resolved package. */
CupError command_install_request(const char *selector,
                                 const char *value,
                                 const char *target_override,
                                 const char *format_override);
CupError command_install(const char *component,
                         const char *selector,
                         const char *target_override,
                         const char *format_override);

/* Remove one installed package and reconcile its default and launchers. */
CupError command_remove(const char *component, const char *selector, const char *target_override);

/* Select one installed package as the default of its exact scope. */
CupError command_default(const char *component, const char *selector, const char *target_override);

/* Show catalog, default, or immutable installed-package information. */
CupError command_search(const char *component, const char *target_override);
CupError command_info(const char *component, const char *target_override);
CupError command_inspect(const char *component, const char *selector, const char *target_override);

/* Update selected installed scopes; cup is updated only by selecting cup. */
CupError command_update(const char *selector);

/* Update the immutable cup release generation through a deferred commit. */
CupError command_update_cup(void);

/* Show or modify install-selection preferences. */
CupError command_config(const char *action,
                        const char *name,
                        const char *value,
                        const char *target_override);

/* CUP assets, diagnosis, recovery, and complete removal. */
CupError command_doctor(void);
CupError command_repair(void);
CupError command_uninstall(int assume_yes);

#endif /* CUP_COMMANDS_H */
