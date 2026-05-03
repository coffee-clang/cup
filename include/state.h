#ifndef CUP_STATE_H
#define CUP_STATE_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

typedef struct {
    char component[MAX_NAME_LEN];
    char platform[MAX_PLATFORM_LEN];
    char entry[MAX_ENTRY_LEN];
} StateEntry;

typedef struct {
    StateEntry installed[MAX_INSTALLED];
    size_t installed_count;

    StateEntry defaults[MAX_DEFAULTS];
    size_t default_count;
} CupState;

// INIT / PERSISTENCE
CupError state_init(CupState *state);
CupError state_load(CupState *state, const char *filename);
CupError state_save(const CupState *state, const char *filename);

// INSTALLED ENTRIES
int state_find_installed(const CupState *state, const char *component, const char *platform, const char *entry);
CupError state_add_installed(CupState *state, const char *component, const char *platform, const char *entry);
CupError state_remove_installed(CupState *state, const char *component, const char *platform, const char *entry);

// DEFAULT ENTRIES
int state_find_default(const CupState *state, const char *component, const char *platform);
CupError state_set_default(CupState *state, const char *component, const char *platform, const char *entry);
const char *state_get_default(const CupState *state, const char *component, const char *platform);

// DEFAULT CLEANUP
CupError state_remove_default_for_component(CupState *state, const char *component, const char *platform);
CupError state_remove_default_if_matches(CupState *state, const char *component, const char *platform, const char *entry);

#endif /* CUP_STATE_H */