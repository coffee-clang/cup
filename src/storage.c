#include <stdio.h>
#include <string.h>

#include <dirent.h>
#include <unistd.h>

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

static void build_toolchain_path(char *buffer, size_t size, const char *toolchain_name){
    snprintf(buffer, size, "%s/%s", TOOLCHAINS_DIR, toolchain_name);
}

static int remove_directory_recursive(const char *path){
    DIR *dir;
    struct dirent *entry;
    char entry_path[512];
    struct stat info;

    dir = opendir(path);
    if(dir == NULL){
        fprintf(stderr, "Error: could not open directory '%s'.\n", path);
        return 1;
    }

    while((entry = readdir(dir)) != NULL){
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
            continue;
        }

        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);

        if(stat(entry_path, &info) != 0){
            fprintf(stderr, "Error: could not access '%s'.\n", entry_path);
            closedir(dir);
            return 1;
        }

        if(S_ISDIR(info.st_mode)){
            if(remove_directory_recursive(entry_path) != 0){
                closedir(dir);
                return 1;
            }
        }
        else {
            if(remove(entry_path) != 0){
                fprintf(stderr, "Error: could not remove file '%s'.\n", entry_path);
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);

    if(rmdir(path) != 0){
        fprintf(stderr, "Error: could not remove directory '%s'.\n", path);
        return 1;
    }

    return 0;
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

int create_toolchain_dir(const char *toolchain_name){
    char path[256];
    struct stat info = {0};

    ensure_cup_structure();
    build_toolchain_path(path, sizeof(path), toolchain_name);

    if(stat(path, &info) == 0){
        return 0;
    }

    if(mkdir(path, 0700) != 0){
        fprintf(stderr, "Error: could not create toolchain directory '%s'.\n", path);
        return 1;
    }

    return 0;
}

int remove_toolchain_dir(const char *toolchain_name){
    char path[256];
    struct stat info;

    build_toolchain_path(path, sizeof(path), toolchain_name);

    if(stat(path, &info) != 0){
        fprintf(stderr, "Error: toolchain directory '%s' does not exist.\n", path);
        return 1;
    }

    if(!S_ISDIR(info.st_mode)){
        fprintf(stderr, "Error: '%s' is not a directory.\n", path);
        return 1;
    }

    return remove_directory_recursive(path);
}

int split_toolchain_name(const char *toolchain_name, char *name, size_t name_size, char *version, size_t version_size){
    const char *at = strchr(toolchain_name, '@');
    size_t name_len;
    size_t version_len;

    if(at == NULL){
        return 1;
    }

    name_len = (size_t)(at - toolchain_name);
    version_len = strlen(at + 1);

    if(name_len == 0 || version_len == 0){
        return 1;
    }

    if(name_len >= name_size || version_len >= version_size){
        return 1;
    }

    strncpy(name, toolchain_name, name_len);
    name[name_len] = '\0';

    strncpy(version, at + 1, version_len);
    version[version_len] = '\0';

    return 0;
}

int write_toolchain_info(const char *toolchain_name){
    char path[256];
    char name[MAX_NAME_LEN];
    char version[MAX_NAME_LEN];
    FILE *file;

    if(split_toolchain_name(toolchain_name, name, sizeof(name), version, sizeof(version)) != 0){
        fprintf(stderr, "Error: invalid toolchain name '%s'.\n", toolchain_name);
        return 1;
    }

    snprintf(path, sizeof(path), "%s/%s/info.txt", TOOLCHAINS_DIR, toolchain_name);

    file = fopen(path, "w");
    if(file == NULL){
        fprintf(stderr, "Error: could not write toolchain info for '%s'.\n", toolchain_name);
        return 1;
    }

    fprintf(file, "name=%s\n", name);
    fprintf(file, "version=%s\n", version);

    fclose(file);
    return 0;
}