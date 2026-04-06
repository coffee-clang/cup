#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "storage.h"

static void trim_newline(char *s){
    size_t len = strlen(s);
    if(len > 0 && s[len - 1] == '\n'){
        s[len - 1] = '\0';
    }
}

static void ensure_directory(const char *path){
    struct stat info = {0};

    if(stat(path, &info) == -1){
        mkdir(path, 0700);
    }
}

static void ensure_cup_structure(void){
    ensure_directory(CUP_DIR);
    ensure_directory(TOOLCHAINS_DIR);
}

void state_init(CupState *state){
    int i;

    state->count = 0;
    state->default_name[0] = '\0';

    for(i = 0; i < MAX_TOOLCHAINS; ++i){
        state->names[i][0] = '\0';
    }
}

int state_load(CupState *state, const char *filename){
    FILE *file;
    char line[256];

    ensure_cup_structure();
    state_init(state);

    file = fopen(filename, "r");
    if(file == NULL){
        return 0;
    }

    while(fgets(line, sizeof(line), file) != NULL){
        trim_newline(line);

        if(strncmp(line, "default=", 8) == 0){
            strncpy(state->default_name, line + 8, MAX_NAME_LEN - 1);
            state->default_name[MAX_NAME_LEN - 1] = '\0';
        } 
        else if(strncmp(line, "toolchain=", 10) == 0){
            if(state->count < MAX_TOOLCHAINS){
                strncpy(state->names[state->count], line + 10, MAX_NAME_LEN);
                state->names[state->count][MAX_NAME_LEN - 1] = '\0';
                state->count++;
            }
        }
    }

    fclose(file);
    return 0;
}

int state_save(const CupState *state, const char *filename){
    FILE *file;
    int i;

    ensure_cup_structure();

    file = fopen(filename, "w");
    if(file == NULL){
        fprintf(stderr, "Error, could not open state file for writing.\n");
        return 1;
    }

    if(state->default_name[0] != '\0'){
        fprintf(file, "default=%s\n", state->default_name);
    }

    for(i = 0; i < state->count; ++i){
        fprintf(file, "toolchain=%s\n", state->names[i]);
    }

    fclose(file);
    return 0;
}

int state_find_toolchain(const CupState *state, const char *name){
    int i;

    for(i = 0; i < state->count; ++i){
        if(strcmp(state->names[i], name) == 0){
            return i;
        }
    }

    return -1;
}

int state_add_toolchain(CupState *state, const char *name){
    if(state->count >= MAX_TOOLCHAINS){
        return 1;
    }

    if(state_find_toolchain(state, name) != -1){
        return 2;
    }

    strncpy(state->names[state->count], name, MAX_NAME_LEN - 1);
    state->names[state->count][MAX_NAME_LEN - 1] = '\0';
    state->count++;

    return 0;
}

int state_remove_toolchain(CupState *state, const char *name){
    int index;
    int i;

    index = state_find_toolchain(state, name);
    if(index == -1){
        return 1;
    }

    for(i = index; i < state->count - 1; ++i){
        strcpy(state->names[i], state->names[i + 1]);
    }

    state->count--;
    state->names[state->count][0] = '\0';

    if(strcmp(state->default_name, name) == 0){
        state->default_name[0] = '\0';
    }

    return 0;
}

int state_set_default(CupState *state, const char *name){
    if(state_find_toolchain(state, name) == -1){
        return 1;
    }
    
    strncpy(state->default_name, name, MAX_NAME_LEN - 1);
    state->default_name[MAX_NAME_LEN - 1] = '\0';

    return 0;
}