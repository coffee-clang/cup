#ifndef CUP_COMMAND_CONTEXT_H
#define CUP_COMMAND_CONTEXT_H

/*
 * Shared command lifetime, platform, lock, state, and catalog resources used by public command
 * handlers.
 */

#include "error.h"
#include "package_catalog.h"
#include "state.h"
#include "system.h"

/* Shared state held for the entire execution of a command. */
typedef struct {
    CupState state;
    PackageCatalog catalog;
    int has_catalog;
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    SystemLock lock;
    int runtime_available;
} CommandContext;

/* Resolve platforms, verify the cup root and acquire the requested lock. */
CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode);

/* Begin a query without creating the cup root when it is absent. */
CupError command_context_begin_read_only(CommandContext *context, const char *target_override);

/* Release the lock and resources owned by the command context. */
void command_context_end(CommandContext *context);

/* Load a present and semantically valid state file. */
CupError command_context_load_state(CommandContext *context);

/* Load the catalog, failing when it is unavailable or invalid. */
CupError command_context_load_catalog(CommandContext *context);

/* Try to load the catalog without failing the command. */
void command_context_try_catalog(CommandContext *context);

#endif /* CUP_COMMAND_CONTEXT_H */
