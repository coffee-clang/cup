#include "filesystem.h"
#include "constants.h"
#include "util.h"
#include "interrupt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef CupError (*DirectoryEntryCallback)(const char *path, const struct stat *info, void *userdata);

static CupError create_directory(const char *path) {
    struct stat info;
    int status;

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "Error: invalid directory path.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    status = stat(path, &info);
    if (status == 0) {
        if (!S_ISDIR(info.st_mode)) {
            fprintf(stderr, "Error: '%s' exists but is not a directory.\n", path);
            return CUP_ERR_FS;
        }

        return CUP_OK;
    }

    if (errno != ENOENT) {
        fprintf(stderr, "Error: could not inspect directory '%s'.\n", path);
        return CUP_ERR_FS;
    }

    status = mkdir(path, 0700);
    if (status != 0) {
        if (errno == EEXIST) {
            status = stat(path, &info);
            if (status == 0 && S_ISDIR(info.st_mode)) {
                return CUP_OK;
            }
        }

        fprintf(stderr, "Error: could not create directory '%s'.\n", path);
        return CUP_ERR_FS;
    }

    return CUP_OK;
}

static CupError walk_directory(const char *path, DirectoryEntryCallback callback, void *userdata) {
    CupError err;
    DIR *dir;
    struct dirent *entry;
    struct stat info;
    char full_path[MAX_PATH_LEN];

    if (path == NULL || path[0] == '\0' || callback == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    dir = opendir(path);
    if (dir == NULL) {
        return CUP_ERR_FS;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (interrupt_requested()) {
            closedir(dir);
            return CUP_ERR_INTERRUPT;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = checked_snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (err != CUP_OK) {
            closedir(dir);
            return err;
        }

        if (lstat(full_path, &info) != 0) {
            closedir(dir);
            return CUP_ERR_FS;
        }

        if (S_ISDIR(info.st_mode)) {
            err = walk_directory(full_path, callback, userdata);
            if (err != CUP_OK) {
                closedir(dir);
                return err;
            }
        } 
        
        err = callback(full_path, &info, userdata);
        if (err != CUP_OK) {
            closedir(dir);
            return err;
        }
    }

    closedir(dir);
    return CUP_OK;
}

static CupError remove_visited_entry(const char *path, const struct stat *info, void *userdata) {
    (void) userdata;

    if (path == NULL || path[0] == '\0' || info == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (S_ISDIR(info->st_mode)) {
        if (rmdir(path) != 0) {
            return CUP_ERR_FS;
        }
    } else {
        if (remove(path) != 0) {
            return CUP_ERR_FS;
        }
    }

    return CUP_OK;
}

static CupError remove_directory_recursive(const char *path) {
    CupError err;
    struct stat info;

    if (path == NULL || path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(path, &info) != 0) {
        return CUP_ERR_FS;
    }

    if (!S_ISDIR(info.st_mode)) {
        return CUP_ERR_FS;
    }

    err = walk_directory(path, remove_visited_entry, NULL);
    if (err != CUP_OK) {
        return err;
    }

    if (rmdir(path) != 0) {
        return CUP_ERR_FS;
    }

    return CUP_OK;
}

static CupError get_cup_root_path(char *buffer, size_t size) {
    CupError err;
    const char *home;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FS;
    }

    err = checked_snprintf(buffer, size, "%s/.cup", home);
    return err;
}

static CupError get_components_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/components", root);
    return err;
}

static CupError get_tmp_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/tmp", root);
    return err;
}

static CupError get_cache_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/cache", root);
    return err;
}

CupError get_state_file_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/state.txt", root);
    return err;
}

CupError ensure_cup_structure(void) {
    CupError err;
    char root[MAX_PATH_LEN];
    char components[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    char cache[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components, sizeof(components));
    if (err != CUP_OK) {
        return err;
    }

    err = get_tmp_root_path(tmp, sizeof(tmp));
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache, sizeof(cache));
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(components);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tmp);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(cache);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError ensure_component_base_dirs(const char *component, const char *tool, const char *platform) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char platform_dir[MAX_PATH_LEN];

    if (component == NULL || tool == NULL || platform == NULL ||
        component[0] == '\0' || tool[0] == '\0' || platform[0] == '\0') {
        fprintf(stderr, "Error: invalid component directory arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(component_dir, sizeof(component_dir), "%s/%s", components_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s", component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(platform_dir, sizeof(platform_dir), "%s/%s", tool_dir, platform);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(component_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tool_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(platform_dir);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char release_dir[MAX_PATH_LEN];

    if (component == NULL || tool == NULL || release == NULL ||
        component[0] == '\0' || tool[0] == '\0' || release[0] == '\0') {
        fprintf(stderr, "Error: invalid component directory arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(component_dir, sizeof(component_dir), "%s/%s", cache_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(tool_dir, sizeof(tool_dir), "%s/%s", component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(release_dir, sizeof(release_dir), "%s/%s", tool_dir, release);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(component_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tool_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(release_dir);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *platform, const char *release) {
    CupError err;
    char components_root[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || platform == NULL || release == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || platform[0] == '\0' || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s/%s/%s/%s", components_root, component, tool, platform, release);
    return err;
}

static CupError build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, long suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0' || suffix == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/install-%s-%s-%s-%ld", tmp_root, component, tool, release, suffix);
    return err;
}

static CupError build_tmp_remove_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, long suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0' || suffix == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/remove-%s-%s-%s-%ld", tmp_root, component, tool, release, suffix);
    return err;
}

static CupError build_install_child_path(char *buffer, size_t size, const char *base_path, const char *relative_path) {
    CupError err;

    if (buffer == NULL || base_path == NULL || relative_path == NULL ||
        size == 0 || base_path[0] == '\0' || relative_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (relative_path[0] == '/') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strstr(relative_path, "..") != NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(buffer, size, "%s/%s", base_path, relative_path);
    return err;
}

static CupError build_cache_package_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_root[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL ||
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s/%s/%s", cache_root, component, tool, release);
    return err;
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *platform, const char *archive_format) {
    CupError err;
    char cache_package_path[MAX_PATH_LEN];

    if (buffer == NULL || component == NULL || tool == NULL || release == NULL || platform == NULL || archive_format == NULL || 
        size == 0 || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0' || platform[0] == '\0' || archive_format[0] == '\0') {
        fprintf(stderr, "Error: invalid archive format.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_cache_package_path(cache_package_path, sizeof(cache_package_path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s-%s-%s.%s", cache_package_path, tool, release, platform, archive_format);
    return err;
}

CupError create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    long suffix;

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    suffix = (long)getpid();

    err = build_tmp_install_path(buffer, size, component, tool, release, suffix);
    if (err != CUP_OK) {
        return CUP_ERR_TMP;
    }

    err = cleanup_tmp_install(buffer);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }
    if (err != CUP_OK) {
        return CUP_ERR_TMP;
    }

    err = create_directory(buffer);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary install directory '%s'.\n", buffer);
        return CUP_ERR_TMP;
    }

    return CUP_OK;
}

CupError create_tmp_remove_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    long suffix;

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    suffix = (long)getpid();

    err = build_tmp_remove_path(buffer, size, component, tool, release, suffix);
    if (err != CUP_OK) {
        return CUP_ERR_TMP;
    }

    err = cleanup_tmp_install(buffer);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }
    if (err != CUP_OK) {
        return CUP_ERR_TMP;
    }

    err = create_directory(buffer);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary remove directory '%s'.\n", buffer);
        return CUP_ERR_TMP;
    }

    return CUP_OK;  
}

static CupError validate_path_type(const char *path, int want_directory) {
    struct stat info;

    if (path == NULL || path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(path, &info) != 0) {
        return CUP_ERR_VALIDATION;
    }

    if (want_directory) {
        if (!S_ISDIR(info.st_mode)) {
            return CUP_ERR_VALIDATION;
        }

        return CUP_OK;
    }

    if (!S_ISREG(info.st_mode)) {
        return CUP_ERR_VALIDATION;
    }

    if (info.st_size <= 0) {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_install_child_path(const char *base_path, const char *relative_path, int want_directory) {
    CupError err;
    char path[MAX_PATH_LEN];

    err = build_install_child_path(path, sizeof(path), base_path, relative_path);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_path_type(path, want_directory);
    return err;
}

CupError validate_install(const char *tmp_path) {
    CupError err;

    if (tmp_path == NULL || tmp_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_path_type(tmp_path, 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: temporary install path '%s' is not a valid directory.\n", tmp_path);
        return CUP_ERR_VALIDATION;
    }

    err = validate_install_child_path(tmp_path, "info.txt", 0);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_install_child_path(tmp_path, "bin", 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package does not contain a bin directory.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_install_child_path(tmp_path, "include", 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package does not contain an include directory.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_install_child_path(tmp_path, "lib", 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package does not contain a lib directory.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_install_child_path(tmp_path, "share", 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package does not contain a share directory.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

CupError commit_path(const char *source_path, const char *destination_path) {
    if (source_path == NULL || destination_path == NULL ||
        source_path[0] == '\0' || destination_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (rename(source_path, destination_path) != 0) {
        fprintf(stderr, "Error: could not move '%s' to '%s'.\n", source_path, destination_path);
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

CupError cleanup_tmp_install(const char *tmp_path) {
    CupError err;
    struct stat info;

    if (tmp_path == NULL || tmp_path[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (lstat(tmp_path, &info) != 0) {
        if (errno == ENOENT) {
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not inspect temporary directory '%s'.\n", tmp_path);
        return CUP_ERR_TMP;
    }

    if (!S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: temporary path '%s' is not directory.\n", tmp_path);
        return CUP_ERR_TMP;
    }

    err = remove_directory_recursive(tmp_path);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }
    if (err != CUP_OK) {
        return CUP_ERR_TMP;
    }

    return CUP_OK;
}

CupError cleanup_all_tmp(void) {
    CupError err;
    DIR *dir;
    struct dirent *entry;
    struct stat info;
    char tmp_root[MAX_PATH_LEN];
    char full_path[MAX_PATH_LEN];

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    dir = opendir(tmp_root);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return CUP_OK;
        }

        return CUP_ERR_TMP;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = checked_snprintf(full_path, sizeof(full_path), "%s/%s", tmp_root, entry->d_name);
        if (err != CUP_OK) {
            continue;
        }

        if (lstat(full_path, &info) != 0) {
            if (errno != ENOENT) {
                fprintf(stderr, "Warning: could not inspect temporary path '%s'.\n", full_path);
            }
            continue;
        }

        if (S_ISDIR(info.st_mode)) {
            err = cleanup_tmp_install(full_path);
            if (err == CUP_ERR_INTERRUPT) {
                closedir(dir);
                return err;
            }
            if (err != CUP_OK) {
                fprintf(stderr, "Warning, could not clean temporary path '%s'.\n", full_path);
            }
        } else {
            if (remove(full_path) != 0 && errno != ENOENT) {
                fprintf(stderr, "Warning: could not remove temporary file '%s'.\n", full_path);
            }
        }
    }

    closedir(dir);
    return CUP_OK;
}

CupError write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *platform, const char *release) {
    CupError err;
    FILE *file;
    char info_path[MAX_PATH_LEN];
    int status;

    if (base_path == NULL || component == NULL || tool == NULL || release == NULL ||
        base_path[0] == '\0' || component[0] == '\0' || tool[0] == '\0' || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", base_path);
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(info_path, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not write info file '%s'.\n", info_path);
        return CUP_ERR_FS;
    }

    status = fprintf(file, "component=%s\n", component);
    if (status < 0) {
        fclose(file);
        return CUP_ERR_INSTALL;
    }
    status = fprintf(file, "tool=%s\n", tool);
    if (status < 0) {
        fclose(file);
        return CUP_ERR_INSTALL;
    }
    status = fprintf(file, "release=%s\n", release);
    if (status < 0) {
        fclose(file);
        return CUP_ERR_INSTALL;
    }
    status = fprintf(file, "platform=%s\n", platform);
    if (status < 0) {
        fclose(file);
        return CUP_ERR_INSTALL;
    }

    status = fclose(file);
    if (status != 0) {
        return CUP_ERR_INSTALL;
    }

    return CUP_OK;
}

CupError installation_exists(const char *component, const char *tool, const char *platform, const char *release, int *exists) {
    CupError err;
    struct stat info;
    char path[MAX_PATH_LEN];

    if (component == NULL || tool == NULL || platform == NULL || release == NULL || exists == NULL ||
        component[0] == '\0' || tool[0] == '\0' || platform[0] == '\0' || release[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    *exists = 0;

    err = build_install_path(path, sizeof(path), component, tool, platform, release);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(path, &info) != 0) {
        if (errno == ENOENT) {
            *exists = 0;
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not inspect installation path '%s'.\n", path);
        return CUP_ERR_FS;
    }

    *exists = S_ISDIR(info.st_mode) ? 1 : 0;
    return CUP_OK;
}

CupError archive_exists(const char *archive_path, int *exists) {
    struct stat info;

    if (archive_path == NULL || archive_path[0] == '\0' || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *exists = 0;

    if (stat(archive_path, &info) != 0) {
        if (errno == ENOENT) {
            *exists = 0;
            return CUP_OK;
        }

        fprintf(stderr, "Error: could not inspect archive '%s'.\n", archive_path);
        return CUP_ERR_FS;
    }

    *exists = S_ISREG(info.st_mode) ? 1 : 0;
    return CUP_OK;
}

CupError archive_is_usable(const char *archive_path, int *is_usable) {
    CupError err;
    struct stat info;
    int exists;

    if (archive_path == NULL || archive_path[0] == '\0' || is_usable == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_usable = 0;

    err = archive_exists(archive_path, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (!exists) {
        return CUP_OK;
    }

    if (stat(archive_path, &info) != 0) {
        fprintf(stderr, "Error: could not inspect archive '%s'.\n", archive_path);
        return CUP_ERR_FS;
    }

    if (info.st_size <= 0) {
        return CUP_OK;
    }

    *is_usable = 1;
    return CUP_OK;
}