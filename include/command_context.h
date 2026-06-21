#ifndef CUP_COMMAND_CONTEXT_H
#define CUP_COMMAND_CONTEXT_H

#include <stdio.h>

#include "error.h"
#include "manifest.h"
#include "package.h"
#include "state.h"
#include "system.h"

/* Parsed command entry before and after resolving a symbolic release. */
typedef struct {
    char tool[MAX_NAME_LEN];
    char release[MAX_NAME_LEN];
    char resolved_release[MAX_NAME_LEN];
    char input_entry[MAX_ENTRY_LEN];
    char resolved_entry[MAX_ENTRY_LEN];
} EntryRequest;

/* Shared state held for the entire execution of a command. */
typedef struct {
    CupState state;
    Manifest manifest;
    int has_manifest;
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    SystemLock lock;
} CommandContext;

/* Resolve platforms, verify the cup root and acquire the requested lock. */
CupError command_context_begin(CommandContext *context,
    const char *target_override, SystemLockMode mode);

/* Release the lock and resources owned by the command context. */
void command_context_end(CommandContext *context);

/* Load a present and semantically valid state file. */
CupError command_context_load_state(CommandContext *context);

/* Load the manifest, failing when it is unavailable or invalid. */
CupError command_context_load_manifest(CommandContext *context);

/* Try to load the manifest without failing the command. */
void command_context_try_manifest(CommandContext *context);

/* Reject normal commands while an interrupted transaction is pending. */
CupError command_require_no_transaction(void);

/* Parse and validate a '<tool>@<release>' command argument. */
CupError entry_request_parse(const char *component, const char *entry, EntryRequest *request);

/* Resolve a symbolic release and build the concrete entry string. */
CupError entry_request_resolve(const Manifest *manifest, const char *component,
    const char *host_platform, const char *target_platform, EntryRequest *request);

/* Print either the original entry or its resolved form. */
void entry_request_print(FILE *stream, const EntryRequest *request);

/* Require a package to be present both in state and on disk. */
CupError command_require_installed(const CommandContext *context, const PackageIdentity *package);

/* Require a package to be absent both from state and disk. */
CupError command_require_absent(const CommandContext *context, const PackageIdentity *package);

#endif /* CUP_COMMAND_CONTEXT_H */
