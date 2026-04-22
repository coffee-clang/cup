#include "fs.h"
#include "fetch.h"
#include "archive.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static CupError create_directory(const char *path) {
    struct stat info;

    if (stat(path, &info) == -1) {
        if (mkdir(path, 0700) != 0) {
            fprintf(stderr, "Error: could not create directory '%s'.\n", path);
            return CUP_ERR_FS;
        }
    }

    return CUP_OK;
}

static CupError remove_directory_recursive(const char *path) {
    DIR *dir;
    struct dirent *entry;
    char full_path[MAX_PATH_LEN];

    dir = opendir(path);
    if (dir == NULL) {
        return CUP_ERR_FS;
    }

    while ((entry = readdir(dir)) != NULL) {
        DIR *subdir;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (checked_snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) != CUP_OK) {
            closedir(dir);
            return CUP_ERR_FS;
        }

        subdir = opendir(full_path);
        if (subdir != NULL) {
            closedir(subdir);

            if (remove_directory_recursive(full_path) != CUP_OK) {
                closedir(dir);
                return CUP_ERR_FS;
            }
        } else {
            if (remove(full_path) != 0) {
                closedir(dir);
                return CUP_ERR_FS;
            }
        }
    }

    closedir(dir);

    if (rmdir(path) != 0) {
        return CUP_ERR_FS;
    }

    return CUP_OK;
}

// Temporary
const char *get_platform_name(void) {
    return "linux";
}

CupError get_cup_root_path(char *buffer, size_t size) {
    const char *home;

    home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "Error: HOME environment variable is not set.\n");
        return CUP_ERR_FS;
    }

    return checked_snprintf(buffer, size, "%s/.cup", home);
}

CupError get_state_file_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/state.txt", root);
}

CupError get_components_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/components", root);
}

CupError get_tmp_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/tmp", root);
}

CupError get_cache_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/cache", root);
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

    if (component == NULL || tool == NULL || platform == NULL) {
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

    if (component == NULL || tool == NULL || release == NULL) {
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

CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char components_root[MAX_PATH_LEN];

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s/%s/%s/%s", components_root, component, tool, get_platform_name(), release);
}

CupError build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, int suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s-%s-%s-%d", tmp_root, component, tool, release, suffix);
}

CupError build_cache_package_path(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    char cache_root[MAX_PATH_LEN];

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/%s/%s/%s", cache_root, component, tool, release);
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *archive_format) {
    CupError err;
    char cache_package_path[MAX_PATH_LEN];

    if (archive_format == NULL || archive_format[0] == '\0') {
        fprintf(stderr, "Error: invalid archive format.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_cache_package_path(cache_package_path, sizeof(cache_package_path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/package.%s", cache_package_path, archive_format);
}

CupError create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release) {
    CupError err;
    int suffix;

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    suffix = (int)getpid();

    err = build_tmp_install_path(buffer, size, component, tool, release, suffix);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(buffer);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary install directory '%s'.\n", buffer);
        return err;
    }

    return CUP_OK;
}

static CupError install_package(const char *package_path, const char *tmp_path, const char *component, const char *tool, const char *resolved_release) {
    CupError err;

    if (package_path == NULL || tmp_path == NULL || component == NULL || tool == NULL || resolved_release == NULL) {
        fprintf(stderr, "Error: invalid package install arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = extract_archive_to_tmp(package_path, tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    err = write_component_info_at_path(tmp_path, component, tool, resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError perform_install(const char *tmp_path, const char *component, const char *tool, const char *release, const char *archive_format) {
    CupError err;
    char package_path[MAX_PATH_LEN];

    err = fetch_package(package_path, sizeof(package_path), component, tool, release, archive_format);
    if (err != CUP_OK) {
        return err;
    }

    err = install_package(package_path, tmp_path, component, tool, release);
    if (err != CUP_OK) {
        return CUP_ERR_INSTALL;
    }

    return CUP_OK;
}

CupError validate_install(const char *tmp_path) {
    CupError err;
    FILE *file;
    struct stat info;
    char info_path[MAX_PATH_LEN];
    int ch;

    if (tmp_path == NULL) {
        fprintf(stderr, "Error: invalid install path.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(tmp_path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: installation validation failed because the temporary directory is missing.\n");
        return CUP_ERR_VALIDATION;
    }

    err = checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(info_path, &info) != 0) {
        fprintf(stderr, "Error: installation validation failed because info.txt is missing.\n");
        return CUP_ERR_VALIDATION;
    }

    file = fopen(info_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: installation validation failed because info.txt could not be opened.\n");
        return CUP_ERR_VALIDATION;
    }

    ch = fgetc(file);
    fclose(file);

    if (ch == EOF) {
        fprintf(stderr, "Error: installation validation failed because info.txt is empty.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

CupError commit_install(const char *tmp_path, const char *final_path) {
    if (tmp_path == NULL || final_path == NULL) {
        fprintf(stderr, "Error: invalid commit arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    if (rename(tmp_path, final_path) != 0) {
        fprintf(stderr, "Error: could not move temporary installation to final destination.\n");
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

CupError cleanup_tmp_install(const char *tmp_path) {
    struct stat info;

    if (tmp_path == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(tmp_path, &info) != 0) {
        return CUP_OK;
    }

    if (!S_ISDIR(info.st_mode)) {
        return CUP_ERR_FS;
    }

    return remove_directory_recursive(tmp_path);
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
        return CUP_OK;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        err = checked_snprintf(full_path, sizeof(full_path), "%s/%s", tmp_root, entry->d_name);
        if (err != CUP_OK) {
            continue;
        }

        if (stat(full_path, &info) != 0) {
            continue;
        }

        if (S_ISDIR(info.st_mode)) {
            cleanup_tmp_install(full_path);
        } else {
            remove(full_path);
        }
    }

    closedir(dir);
    return CUP_OK;
}

CupError write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *release) {
    CupError err;
    FILE *file;
    char info_path[MAX_PATH_LEN];

    err = checked_snprintf(info_path, sizeof(info_path), "%s/info.txt", base_path);
    if (err != CUP_OK) {
        return err;
    }

    file = fopen(info_path, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: could not write info file '%s'.\n", info_path);
        return CUP_ERR_FS;
    }

    fprintf(file, "component=%s\n", component);
    fprintf(file, "tool=%s\n", tool);
    fprintf(file, "release=%s\n", release);
    fprintf(file, "platform=%s\n", get_platform_name());

    fclose(file);
    return CUP_OK;
}

CupError installation_exists(const char *component, const char *tool, const char *release, int *exists) {
    CupError err;
    struct stat info;
    char path[MAX_PATH_LEN];

    if (exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_install_path(path, sizeof(path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
        *exists = 1;
    } else {
        *exists = 0;
    }

    return CUP_OK;
}

CupError archive_exists(const char *archive_path, int *exists) {
    struct stat info;

    if (archive_path == NULL || exists == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (stat(archive_path, &info) == 0 && S_ISREG(info.st_mode)) {
        *exists = 1;
    } else {
        *exists = 0;
    }

    return CUP_OK;
}

CupError remove_component_install_dir(const char *component, const char *tool, const char *release) {
    CupError err;
    struct stat info;
    char path[MAX_PATH_LEN];

    err = build_install_path(path, sizeof(path), component, tool, release);
    if (err != CUP_OK) {
        return err;
    }

    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "Error: installation directory does not exist.\n");
        return CUP_ERR_NOT_INSTALLED;
    }

    err = remove_directory_recursive(path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not remove installation directory '%s'.\n", path);
        return err;
    }

    return CUP_OK;
}