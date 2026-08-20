#ifndef CUP_COMMANDS_H
#define CUP_COMMANDS_H

/*
 * Public CLI command handlers receive canonical arguments from the parser. They own runtime
 * policy/state validation, locking, output and state transitions; they do not reinterpret public
 * grammar.
 */

#include "error.h"

/* List installed packages, optionally restricted by component or target. */
CupError command_list(const char *component, const char *target_override);

/* Install one canonical package request, profile or curated toolchain after complete preflight. */
CupError command_install(const char *selector,
                         const char *value,
                         const char *target_override,
                         const char *format_override);

/* Remove one installed package; the canonical selector may omit release when it is unambiguous. */
CupError command_remove(const char *component, const char *selector, const char *target_override);

/* Select one installed package as the default of its exact scope. */
CupError command_default(const char *component, const char *selector, const char *target_override);

/* Show packages available in the current catalog. */
CupError command_search(const char *component, const char *target_override);

/* Show defaults and the commands exposed by them. */
CupError command_info(const char *component, const char *target_override);

/* Show validated metadata for one installed package. */
CupError command_inspect(const char *component, const char *selector, const char *target_override);

/* Update selected installed scopes; cup is updated only by selecting cup. */
CupError command_update(const char *selector);

/* Show or modify install-selection preferences. */
CupError command_config(const char *action,
                        const char *name,
                        const char *value,
                        const char *target_override);

/* Diagnose the current installation without modifying it. */
CupError command_doctor(void);

/* Recover interrupted operations and safely derivable installation data. */
CupError command_repair(void);

/* Remove the selected cup installation after optional confirmation. */
CupError command_uninstall(int assume_yes);

#endif /* CUP_COMMANDS_H */
