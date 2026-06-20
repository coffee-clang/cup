#ifndef CUP_STATE_H
#define CUP_STATE_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef struct {
    char component[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char entry[MAX_ENTRY_LEN];
} StateEntry;

typedef struct {
    StateEntry installed[MAX_INSTALLED];
    size_t installed_count;

    StateEntry defaults[MAX_DEFAULTS];
    size_t default_count;
} CupState;

typedef enum {
    STATE_FILE_MISSING,
    STATE_FILE_LOADED
} StateFileStatus;

/* Load, validate and atomically save state.txt. */
CupError state_load(CupState *state, StateFileStatus *status);
CupError state_validate(const CupState *state);
CupError state_save(const CupState *state);

/* Installed entries. */
int state_find_installed(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);
CupError state_add_installed(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);
CupError state_remove_installed(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);

/* Default entries. */
int state_find_default(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform);
const char *state_get_default(const CupState *state, const char *component,
    const char *host_platform, const char *target_platform);
CupError state_set_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);
CupError state_clear_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform);
CupError state_clear_matching_default(CupState *state, const char *component,
    const char *host_platform, const char *target_platform, const char *entry);

#endif /* CUP_STATE_H */
