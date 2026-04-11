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

    if (buffer == NULL || format == NULL || size == 0) {
        fprintf(stderr, "Error: invalid buffer or format in checked_snprintf.\n");
        return 1;
    }

    va_start(args, format);
    written = vsnprintf(buffer, size, format, args);
    va_end(args);

    if (written < 0) {
        fprintf(stderr, "Error: could not format string.\n");
        return 1;
    }

    if ((size_t)written >= size) {
        fprintf(stderr, "Error: formatted string is too long.\n");
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

    if (home == NULL) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return 1;
    }

    return checked_snprintf(buffer, size, "%s/.cup", home);
}

int get_state_file_path(char *buffer, size_t size) {
    char root[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) {
        return 1;
    }

    return checked_snprintf(buffer, size, "%s/state.txt", root);
}

static int create_directory(const char *path) {
    struct stat info = {0};

    if (stat(path, &info) == -1) {
        if (mkdir(path, 0700) != 0) {
            fprintf(stderr, "Error: could not create directory '%s'.\n", path);
            return 1;
        }
    }

    return 0;
}

int ensure_cup_structure(void) {
    char root[MAX_PATH_LEN];
    char components[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) {
        return 1;
    }

    if (checked_snprintf(components, sizeof(components), "%s/components", root) != 0) {
        return 1;
    }

    if (checked_snprintf(tmp, sizeof(tmp), "%s/tmp", root) != 0) {
        return 1;
    }

    if (create_directory(root) != 0) {
        return 1;
    }

    if (create_directory(components) != 0) {
        return 1;
    }

    if (create_directory(tmp) != 0) {
        return 1;
    }

    return 0;
}

static int create_component_dirs(const char *component, const char *tool, const char *platform) {
    char root[MAX_PATH_LEN];
    char components[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char platform_dir[MAX_PATH_LEN];

    if (get_cup_root_path(root, sizeof(root)) != 0) {
        return 1;
    }

    if (checked_snprintf(components, sizeof(components), "%s/components", root) != 0) {
        return 1;
    }

    if (checked_snprintf(component_dir, sizeof(component_dir), "%s/%s",
                         components, component) != 0) {
        return 1;
    }

    if (checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s",
                         component_dir, tool) != 0) {
        return 1;
    }

    if (checked_snprintf(platform_dir, sizeof(platform_dir), "%s/%s",
                         tool_dir, platform) != 0) {
        return 1;
    }

    if (ensure_cup_structure() != 0) {
        return 1;
    }

    if (create_directory(component_dir) != 0) {
        return 1;
    }

    if (create_directory(tool_dir) != 0) {
        return 1;
    }

    if (create_directory(platform_dir) != 0) {
        return 1;
    }

    return 0;
}

static int build_component_release_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    char root[MAX_PATH_LEN];
    const char *platform = get_platform_name();

    if (get_cup_root_path(root, sizeof(root)) != 0) {
        return 1;
    }

    return checked_snprintf(buffer, size, "%s/components/%s/%s/%s/%s", root, component, tool, platform, release);
}

static int remove_directory_recursive(const char *path) {
    DIR *dir;
    struct dirent *entry;
    char entry_path[MAX_PATH_LEN];
    struct stat info;

    dir = opendir(path);
    if (dir == NULL) {
        fprintf(stderr, "Error: could not open directory '%s'.\n", path);
        return 1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (checked_snprintf(entry_path, sizeof(entry_path), "%s/%s",
                             path, entry->d_name) != 0) {
            closedir(dir);
            return 1;
        }

        if (stat(entry_path, &info) != 0) {
            fprintf(stderr, "Error: could not access '%s'.\n", entry_path);
            closedir(dir);
            return 1;
        }

        if (S_ISDIR(info.st_mode)) {
            if (remove_directory_recursive(entry_path) != 0) {
                closedir(dir);
                return 1;
            }
        } else {
            if (remove(entry_path) != 0) {
                fprintf(stderr, "Error: could not remove file '%s'.\n", entry_path);
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);

    if (rmdir(path) != 0) {
        fprintf(stderr, "Error: could not remove directory '%s'.\n", path);
        return 1;
    }

    return 0;
}

int create_component_installation_dir(const char *component, const char *tool, const char *release) {
    char release_path[MAX_PATH_LEN];

    if (create_component_dirs(component, tool, get_platform_name()) != 0) {
        return 1;
    }

    if (build_component_release_path(release_path, sizeof(release_path), component, tool, release) != 0) {
        return 1;
    }

    return create_directory(release_path);
}

int remove_component_installation_dir(const char *component, const char *tool, const char *release) {
    char release_path[MAX_PATH_LEN];
    struct stat info;

    if (build_component_release_path(release_path, sizeof(release_path), component, tool, release) != 0) {
        return 1;
    }

    if (stat(release_path, &info) != 0) {
        fprintf(stderr, "Error: installation path '%s' does not exist.\n", release_path);
        return 1;
    }

    if (!S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a directory.\n", release_path);
        return 1;
    }

    return remove_directory_recursive(release_path);
}

int write_component_info(const char *component, const char *tool, const char *release) {
    char release_path[MAX_PATH_LEN];
    char info_path[MAX_PATH_LEN];
    FILE *file;

    if (build_component_release_path(release_path, sizeof(release_path), component, tool, release) != 0) {
        return 1;
    }

    if (checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", release_path) != 0) {
        return 1;
    }

    file = fopen(info_path, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not write component info.\n");
        return 1;
    }

    fprintf(file, "component=%s\n", component);
    fprintf(file, "tool=%s\n", tool);
    fprintf(file, "release=%s\n", release);
    fprintf(file, "platform=%s\n", get_platform_name());

    fclose(file);
    return 0;
}