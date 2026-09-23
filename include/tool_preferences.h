#ifndef CUP_TOOL_PREFERENCES_H
#define CUP_TOOL_PREFERENCES_H

/*
 * Atomically persisted scoped user tool preferences layered over immutable official
 * installation defaults.
 */

#include <stddef.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "install_policy.h"
#include "package.h"

typedef struct {
    PackageScope scope;
    char tool[MAX_IDENTIFIER_LEN];
} ToolPreference;

typedef struct {
    ToolPreference items[MAX_TOOL_PREFERENCES];
    size_t count;
} ToolPreferences;

/* Load/save the complete preferences document. The caller's exclusive root lock serializes
 * persistence; preferences do not use the state transaction protocol. */
void tool_preferences_init(ToolPreferences *preferences);
CupError tool_preferences_load(ToolPreferences *preferences, FILE *diagnostics);
CupError tool_preferences_save(const ToolPreferences *preferences);

/* Update one scope in memory; persist only after validation succeeds. The model must be
 * initialized or loaded through this API. */
CupError tool_preferences_set(ToolPreferences *preferences,
                              const char *host_platform,
                              const char *target_platform,
                              const char *component,
                              const char *tool);
CupError tool_preferences_reset(ToolPreferences *preferences,
                                const char *host_platform,
                                const char *target_platform,
                                const char *component,
                                int *removed);
CupError tool_preferences_reset_scope(ToolPreferences *preferences,
                                      const char *host_platform,
                                      const char *target_platform,
                                      size_t *removed_count);

/* Return the user preference for one local target/component scope, or NULL when absent. */
const ToolPreference *tool_preferences_find(const ToolPreferences *preferences,
                                            const char *target_platform,
                                            const char *component);

#endif /* CUP_TOOL_PREFERENCES_H */
