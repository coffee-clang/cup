#include "filesystem.h"
#include "constants.h"
#include "interrupt.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static CupError create_directory(const char *path) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(path)) {
        fprintf(stderr, "Error: invalid directory path.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (exists) {
        err = system_is_directory(path, &is_directory);
        if (err != CUP_OK) {
            return CUP_ERR_FILESYSTEM;
        }

        if (!is_directory) {
            fprintf(stderr, "Error: '%s' exists but is not a directory.\n", path);
            return CUP_ERR_FILESYSTEM;
        }

        return CUP_OK;
    }

    err = system_make_directory(path);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError remove_visited_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    (void) userdata;

    if (info == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (info->is_directory) {
        return system_remove_directory(path);
    }

    if (remove(path) != 0) {
        fprintf(stderr, "Error: could not remove file '%s'.\n", path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError remove_directory_recursive(const char *path) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!exists) {
        return CUP_OK;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (!is_directory) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_walk_directory(path, remove_visited_entry, NULL);
    if (err != CUP_OK) {
        return err;
    }

    err = system_remove_directory(path);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError get_cup_root_path(char *buffer, size_t size) {
    CupError err;
    char home[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err != CUP_OK) {
        return err;
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

CupError ensure_component_base_dirs(const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char host_dir[MAX_PATH_LEN];
    char target_dir[MAX_PATH_LEN];

    if (is_empty_string(component) || is_empty_string(tool) || is_empty_string(host_platform) || is_empty_string(target_platform)) {
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

    err = checked_snprintf(host_dir, sizeof(host_dir), "%s/%s", tool_dir, host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(target_dir, sizeof(target_dir), "%s/%s", host_dir, target_platform);
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

    err = create_directory(host_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(target_dir);
    return err;
}

CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *version) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    char component_dir[MAX_PATH_LEN];
    char tool_dir[MAX_PATH_LEN];
    char version_dir[MAX_PATH_LEN];

    if (is_empty_string(component) || is_empty_string(tool) || is_empty_string(version)) {
        fprintf(stderr, "Error: invalid cache directory arguments.\n");
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

    err = checked_snprintf(version_dir, sizeof(version_dir), "%s/%s", tool_dir, version);
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

    err = create_directory(version_dir);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *version) {
    CupError err;
    char components_root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s/%s/%s/%s/%s", components_root, component, tool, host_platform, target_platform, version);
    return err;
}

static CupError build_tmp_path(char *buffer, size_t size, const char *operation, const char *component, const char *tool, 
    const char *version, const char *suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(operation) || is_empty_string(component) || 
        is_empty_string(tool) || is_empty_string(version) || is_empty_string(suffix)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s-%s-%s-%s-%s", tmp_root, operation, component, tool, version, suffix);
    return err;
}

static CupError build_install_child_path(char *buffer, size_t size, const char *base_path, const char *relative_path) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(base_path) || is_empty_string(relative_path)) {
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

static CupError build_cache_package_path(char *buffer, size_t size, const char *component, const char *tool, const char *version) {
    CupError err;
    char cache_root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s/%s/%s", cache_root, component, tool, version);
    return err;
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *version, 
    const char *archive_name) {
    CupError err;
    char cache_package_path[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(version) || is_empty_string(archive_name)) {
        fprintf(stderr, "Error: invalid archive format.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_cache_package_path(cache_package_path, sizeof(cache_package_path), component, tool, version);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s", cache_package_path, archive_name);
    return err;
}

CupError create_tmp_dir(char *buffer, size_t size, const char *operation, const char *component, const char *tool, 
    const char *version) {
    CupError err;
    char suffix[MAX_NAME_LEN];

    err = ensure_cup_structure();
    if (err != CUP_OK) {
        return err;
    }

    err = system_get_process_id(suffix, sizeof(suffix));
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = build_tmp_path(buffer, size, operation, component, tool, version, suffix);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = cleanup_tmp_path(buffer);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }

    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = create_directory(buffer);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not create temporary install directory '%s'.\n", buffer);
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

static CupError validate_path_type(const char *path, int want_directory) {
    CupError err;
    int matches_type;
    long long size;

    if (is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (want_directory) {
        err = system_is_directory(path, &matches_type);
        if (err != CUP_OK || !matches_type) {
            return CUP_ERR_VALIDATION;
        }

        return CUP_OK;
    }

    err = system_is_regular_file(path, &matches_type);
    if (err != CUP_OK || !matches_type) {
        return CUP_ERR_VALIDATION;
    }

    err = system_file_size(path, &size);
    if (err != CUP_OK || size <= 0) {
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

    if (is_empty_string(tmp_path)) {
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

    return CUP_OK;
}

CupError commit_path(const char *source_path, const char *destination_path) {
    CupError err;

    if (is_empty_string(source_path) || is_empty_string(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_rename_path(source_path, destination_path);
    if (err != CUP_OK) {
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

CupError cleanup_tmp_path(const char *tmp_path) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(tmp_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(tmp_path, &exists);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not inspect temporary directory '%s'.\n", tmp_path);
        return CUP_ERR_TEMPORARY;
    }

    if (!exists) {
        return CUP_OK;
    }

    err = system_is_directory(tmp_path, &is_directory);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    if (!is_directory) {
        fprintf(stderr, "Error: temporary path '%s' is not directory.\n", tmp_path);
        return CUP_ERR_TEMPORARY;
    }

    err = remove_directory_recursive(tmp_path);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }

    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

CupError cleanup_all_tmp(void) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];
    int exists;
    int is_directory;

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = system_path_exists(tmp_root, &exists);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    if (!exists) {
        return CUP_OK;
    }

    err = system_is_directory(tmp_root, &is_directory);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    if (!is_directory) {
        return CUP_ERR_TEMPORARY;
    }

    err = system_walk_directory(tmp_root, remove_visited_entry, NULL);
    if (err == CUP_ERR_INTERRUPT) {
        return err;
    }

    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}

CupError installation_exists(const char *component, const char *tool, const char *host_platform, const char *target_platform, 
    const char *version, int *exists) {
    CupError err;
    char path[MAX_PATH_LEN];
    int is_directory;

    if (exists == NULL || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *exists = 0;

    err = build_install_path(path, sizeof(path), component, tool, host_platform, target_platform, version);
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    *exists = is_directory ? 1 : 0;
    return CUP_OK;
}

CupError archive_exists(const char *archive_path, int *exists) {
    CupError err;
    int is_regular_file;

    if (exists == NULL || is_empty_string(archive_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    *exists = 0;

    err = system_is_regular_file(archive_path, &is_regular_file);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    *exists = is_regular_file ? 1 : 0;
    return CUP_OK;
}

CupError archive_is_usable(const char *archive_path, int *is_usable) {
    CupError err;
    int exists;
    long long size;

    if (is_usable == NULL || is_empty_string(archive_path)) {
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

    err = system_file_size(archive_path, &size);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    if (size <= 0) {
        return CUP_OK;
    }

    *is_usable = 1;
    return CUP_OK;
}