#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "fs.h"

int checked_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int written;

    if (!buffer || !format || size == 0) {
        fprintf(stderr, "Error: invalid snprintf arguments.\n");
        return 1;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "Error: path too long.\n");
        return 1;
    }

    return 0;
}

/* Temporary stub: platform detection will be improved later. */
const char *get_platform_name(void) {
    return "linux";
}

int get_cup_root_path(char *buffer, size_t size) {
    const char *home = getenv("HOME");

    if (!home) {
        fprintf(stderr, "Error: HOME not set.\n");
        return 1;
    }

    return checked_snprintf(buffer, size, "%s/.cup", home);
}

int get_tmp_root_path(char *buffer, size_t size) {
    char root[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) 
        return 1;

    return checked_snprintf(buffer, size, "%s/tmp", root);
}

int get_components_root_path(char *buffer, size_t size) {
    char root[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) 
        return 1;

    return checked_snprintf(buffer, size, "%s/components", root);
}

int get_state_file_path(char *buffer, size_t size) {
    char root[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) 
        return 1;

    return checked_snprintf(buffer, size, "%s/state.txt", root);
}

static int create_directory(const char *path) {
    struct stat info;

    if (stat(path, &info) == -1) {
        if (mkdir(path, 0700) != 0) {
            fprintf(stderr, "Error: cannot create dir '%s'.\n", path);
            return 1;
        }
    }

    return 0;
}

int ensure_cup_structure(void) {
    char root[MAX_PATH_LEN];
    char components[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) 
        return 1;

    if (get_components_root_path(components, sizeof(components)) != 0) 
        return 1;

    if (get_tmp_root_path(tmp, sizeof(tmp)) != 0) 
        return 1;

    if (create_directory(root) != 0) 
        return 1;

    if (create_directory(components) != 0) 
        return 1;

    if (create_directory(tmp) != 0) 
        return 1;

    return 0;
}

int ensure_component_dirs(const char *component, const char *tool, const char *platform) {
    char components[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char platform_dir[MAX_PATH_LEN];
    
    if (!component || !tool || !platform) {
        fprintf(stderr, "Error: invalid arguments for component dirs.\n");
        return 1;
    }

    if (ensure_cup_structure() != 0) 
        return 1;

    if (get_components_root_path(components, sizeof(components)) != 0) 
        return 1;

    if (checked_snprintf(component_dir, sizeof(component_dir), "%s/%s", components, component) != 0) 
        return 1;

    if (checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s", component_dir, tool) != 0) 
        return 1;

    if (checked_snprintf(platform_dir, sizeof(platform_dir), "%s/%s", tool_dir, platform) != 0) 
        return 1;

    if (create_directory(component_dir) != 0) 
        return 1;

    if (create_directory(tool_dir) != 0) 
        return 1;

    if (create_directory(platform_dir) != 0) 
        return 1;

    return 0;
}

int build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    char root[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) 
        return 1;

    return checked_snprintf(buffer, size, "%s/components/%s/%s/%s/%s", root, component, tool, get_platform_name(), release);
}

int build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, int suffix) {
    char tmp_root[MAX_PATH_LEN];

    if (get_tmp_root_path(tmp_root, sizeof(tmp_root)) != 0) 
        return 1;

    return checked_snprintf(buffer, size, "%s/%s-%s-%s-%d", tmp_root, component, tool, release, suffix);
}

int create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    int suffix = (int)getpid();

    if (ensure_cup_structure() != 0) 
        return 1;

    if (build_tmp_install_path(buffer, size, component, tool, release, suffix) != 0) 
        return 1;

    if (create_directory(buffer) != 0) 
        return 1;

    return 0;
}

int write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *release) {
    char path[MAX_PATH_LEN];
    FILE *file;

    if (checked_snprintf(path, sizeof(path), "%s/info.txt", base_path) != 0) 
        return 1;

    file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Error: cannot write info file.\n");
        return 1;
    }

    fprintf(file, "component=%s\n", component);
    fprintf(file, "tool=%s\n", tool);
    fprintf(file, "release=%s\n", release);
    fprintf(file, "platform=%s\n", get_platform_name());

    fclose(file);
    return 0;
}

int simulate_install(const char *tmp_path, const char *component, const char *tool, const char *release) {
    char bin[MAX_PATH_LEN];

    if (checked_snprintf(bin, sizeof(bin), "%s/bin", tmp_path) != 0) 
        return 1;

    if (create_directory(bin) != 0) 
        return 1;

    return write_component_info_at_path(tmp_path, component, tool, release);
}

int validate_install(const char *tmp_path) {
    char info_path[MAX_PATH_LEN];
    struct stat info;

    if (checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", tmp_path) != 0) 
        return 1;

    if (stat(info_path, &info) != 0) {
        fprintf(stderr, "Error: invalid install (missing info.txt).\n");
        return 1;
    }

    return 0;
}

int commit_install(const char *tmp_path, const char *final_path) {
    
    if (rename(tmp_path, final_path) != 0) {
        fprintf(stderr, "Error: commit failed.\n");
        return 1;
    }

    return 0;
}

static int remove_directory_recursive(const char *path) {
    DIR *dir;
    struct dirent *entry;
    char full[MAX_PATH_LEN];
    struct stat info;

    dir = opendir(path);
    if (!dir) return 1;

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        if (checked_snprintf(full, sizeof(full), "%s/%s", path, entry->d_name) != 0) {
            closedir(dir);
            return 1;
        }

        if (stat(full, &info) != 0) {
            closedir(dir);
            return 1;
        }

        if (S_ISDIR(info.st_mode)) {
            if (remove_directory_recursive(full) != 0) {
                closedir(dir);
                return 1;
            }
        } 
        else {
            if (remove(full) != 0) {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return rmdir(path);
}

int cleanup_tmp_install(const char *tmp_path) {
    struct stat info;

    if (stat(tmp_path, &info) != 0) 
        return 0;

    if (!S_ISDIR(info.st_mode)) 
        return 1;

    return remove_directory_recursive(tmp_path);
}

int remove_component_install_dir(const char *component, const char *tool, const char *release) {
    char path[MAX_PATH_LEN];
    struct stat info;

    if (build_install_path(path, sizeof(path), component, tool, release) != 0) return 1;

    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: install not found.\n");
        return 1;
    }

    return remove_directory_recursive(path);
}