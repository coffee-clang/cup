#ifndef STATE_H
#define STATE_H

#include <stddef.h>

#include "error.h"

#define MAX_INSTALLED 32
#define MAX_DEFAULTS 6
#define MAX_NAME_LEN 64
#define MAX_ENTRY_LEN 128
#define MAX_PATH_LEN 512

typedef struct {
    char component[MAX_NAME_LEN];
    char entry[MAX_ENTRY_LEN];
} ComponentEntry;

typedef struct {
    ComponentEntry installed[MAX_INSTALLED];
    int installed_count;

    ComponentEntry defaults[MAX_DEFAULTS];
    int default_count;
} CupState;

// INIT / PERSISTENCE
void state_init(CupState *state);
CupError state_load(CupState *state, const char *filename);
CupError state_save(const CupState *state, const char *filename);

// INSTALLED ENTRIES
int state_find_installed(const CupState *state, const char *component, const char *entry);
CupError state_add_installed(CupState *state, const char *component, const char *entry);
CupError state_remove_installed(CupState *state, const char *component, const char *entry);

// DEFAULT ENTRIES
int state_find_default(const CupState *state, const char *component);
CupError state_set_default(CupState *state, const char *component, const char *entry);
const char *state_get_default(const CupState *state, const char *component);

// DEFAULT CLEANUP
void state_remove_default_for_component(CupState *state, const char *component);
void state_remove_default_if_matches(CupState *state, const char *component, const char *entry);

#endif