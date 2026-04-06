#ifndef STORAGE_H
#define STORAGE_H

#define MAX_TOOLCHAINS 32
#define MAX_NAME_LEN 64
#define CUP_DIR ".cup"
#define TOOLCHAINS_DIR ".cup/toolchains"
#define STATE_FILE ".cup/state.txt"

typedef struct {
    char names[MAX_TOOLCHAINS][MAX_NAME_LEN];
    int count;
    char default_name[MAX_NAME_LEN];
} CupState;

void state_init(CupState *state);
int state_load(CupState *state, const char *filename);
int state_save(const CupState *state, const char *filename);

int state_find_toolchain(const CupState *state, const char *name);
int state_add_toolchain(CupState *state, const char *name);
int state_remove_toolchain(CupState *state, const char *name);
int state_set_default(CupState *state, const char *name);

#endif