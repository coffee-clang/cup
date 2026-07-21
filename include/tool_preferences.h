#ifndef CUP_TOOL_PREFERENCES_H
#define CUP_TOOL_PREFERENCES_H

/*
 * Atomically persisted scoped user tool preferences layered over immutable official
 * installation defaults.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "install_policy.h"
#include "package.h"

typedef struct {
    PackageScope scope;
    char tool[MAX_IDENTIFIER_LEN];
} ToolPreference;

typedef struct {
    ToolPreference items[MAX_INSTALL_DEFAULTS];
    size_t count;
} ToolPreferences;

typedef enum {
    TOOL_PREFERENCE_NONE,
    TOOL_PREFERENCE_USER,
    TOOL_PREFERENCE_OFFICIAL_DEFAULT
} ToolPreferenceSource;

/* Load or atomically save the complete preferences document. */
void tool_preferences_init(ToolPreferences *preferences);
CupError tool_preferences_load(const InstallPolicy *policy, ToolPreferences *preferences);
CupError tool_preferences_save(const InstallPolicy *policy, const ToolPreferences *preferences);
CupError tool_preferences_reset_all(void);

/* Mutate one scope in memory; callers persist only after all validation succeeds. */
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

/* Apply user preference > official default precedence for one exact scope. */
CupError tool_preferences_resolve(const InstallPolicy *policy,
                                  const ToolPreferences *preferences,
                                  const char *host_platform,
                                  const char *target_platform,
                                  const char *component,
                                  char *tool,
                                  size_t tool_size,
                                  ToolPreferenceSource *source);

#endif /* CUP_TOOL_PREFERENCES_H */
