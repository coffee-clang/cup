#include <stdio.h>
#include <string.h>
#include "toolchain.h"
#include "storage.h"

static int is_valid_toolchain_name(const char *name){
    const char *at = strchr(name, '@');

    if(at == NULL){
        return 0;
    }

    if(at == name){
        return 0;
    }

    if(*(at + 1) == '\0'){
        return 0;
    }

    if(strchr(at + 1, '@') != NULL){
        return 0;
    }

    return 1;
}

int handle_list(void){
    CupState state;
    int i;
    
    state_load(&state, STATE_FILE);

    if(state.count == 0){
        printf("No toolchains installed yet.\n");
        return 0;
    }

    printf("Installed toolchains:\n");
    for(i = 0; i < state.count; ++i){
        printf("- %s", state.names[i]);

        if(strcmp(state.names[i], state.default_name) == 0){
            printf(" (default)");
        }

        printf("\n");
    }

    return 0;
    
}

int handle_install(const char *name){
    CupState state;
    int result;

    if(!is_valid_toolchain_name(name)){
        fprintf(stderr, "Error: invalid toolchain format. Use <name>@<version>.\n");
        return 1;
    }

    state_load(&state, STATE_FILE);

    result = state_add_toolchain(&state, name);
    if(result == 1){
        fprintf(stderr, "Error, maximum number of toolchain reached.\n");
        return 1;
    }

    if(result == 2){
        fprintf(stderr, "Error, toolchain '%s' is already installed.\n", name);
        return 1;
    }

    if(create_toolchain_dir(name) != 0){
        return 1;
    }

    if(state_save(&state, STATE_FILE) != 0){
        return 1;
    }

    printf("Toolchain '%s' installed successfully.\n", name);
    return 0;
}

int handle_remove(const char *name){
    CupState state;
    int result;

    if(!is_valid_toolchain_name(name)){
        fprintf(stderr, "Error: invalid toolchain format. Use <name>@<version>.\n");
        return 1;
    }

    state_load(&state, STATE_FILE);

    result = state_remove_toolchain(&state, name);
    if(result != 0){
        fprintf(stderr, "Error: toolchain '%s' is not installed.\n", name);
        return 1;
    }

    if(remove_toolchain_dir(name) != 0){
        return 1;
    }

    if(state_save(&state, STATE_FILE) != 0){
        return 1;
    }

    printf("Toolchain '%s' removed successfully.\n", name);
    return 0;
}

int handle_default(const char *name){
    CupState state;
    int result;

    if(!is_valid_toolchain_name(name)){
        fprintf(stderr, "Error: invalid toolchain format. Use <name>@<version>.\n");
        return 1;
    }

    state_load(&state, STATE_FILE);

    result = state_set_default(&state, name);
    if(result != 0){
        fprintf(stderr, "Error: toolchain '%s' is not installed.\n", name);
        return 1;
    }

    if(state_save(&state, STATE_FILE) != 0){
        return 1;
    }

    printf("Default toolchain set to '%s'.\n", name);
    return 0;
}

int handle_current(void){
    CupState state;

    state_load(&state, STATE_FILE);

    if(state.default_name[0] == '\0'){
        printf("No default toolchain set.\n");
        return 0;
    }

    printf("Current default toolchain: %s\n", state.default_name);
    return 0;
}

