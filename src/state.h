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

void state_init(CupState *state);
CupError state_load(CupState *state, const char *filename);
CupError state_save(const CupState *state, const char *filename);

int state_find_installed(const CupState *state, const char *component, const char *entry);
CupError state_add_installed(CupState *state, const char *component, const char *entry);
CupError state_remove_installed(CupState *state, const char *component, const char *entry);

int state_find_default(const CupState *state, const char *component);
CupError state_set_default(CupState *state, const char *component, const char *entry);
const char *state_get_default(const CupState *state, const char *component);

void state_remove_default_component(CupState *state, const char *component);
void state_remove_default(CupState *state, const char *component, const char *entry);

#endif