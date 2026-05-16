#include "filesystem.h"

#include "constants.h"
#include "interrupt.h"
#include "path.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

// DIRECTORY HELPERS
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

static CupError count_visited_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    size_t *count;

    (void) path;
    (void) info;

    if (userdata == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    count = userdata;
    (*count)++;

    return CUP_OK;
}

static CupError remove_visited_entry(const char *path, const SystemPathInfo *info, void *userdata) {
    CupError err;
    (void) userdata;

    if (info == NULL || is_empty_string(path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (info->is_directory) {
        err = system_remove_directory(path);
        return err;
    }

    err = system_remove_file(path);
    return err;
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

// ROOT PATHS
CupError get_cup_root_path(char *buffer, size_t size) {
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

    err = path_join(buffer, size, root, "components");
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

    err = path_join(buffer, size, root, "tmp");
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

    err = path_join(buffer, size, root, "cache");
    return err;
}

static CupError get_config_root_path(char *buffer, size_t size) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, root, "config");
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

    err = path_join(buffer, size, root, "state.txt");
    return err;
}

CupError get_uninstall_script_path(char *buffer, size_t size) {
    CupError err;
    char cup_root[MAX_PATH_LEN];
    char scripts_dir[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(cup_root, sizeof(cup_root));
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(scripts_dir, sizeof(scripts_dir), cup_root, "scripts");
    if (err != CUP_OK) {
        return err;
    }

#ifdef _WIN32
    err = path_join(buffer, size, scripts_dir, "uninstall.ps1");
#else
    err = path_join(buffer, size, scripts_dir, "uninstall.sh");
#endif

    return err;
}

// STRUCTURE
static CupError check_required_directory(const char *path, const char *label, size_t *missing_count) {
    CupError err;
    int is_directory;

    if (is_empty_string(path) || is_empty_string(label) || missing_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: could not inspect %s directory '%s'.\n", label, path);
        (*missing_count)++;
        return CUP_OK;
    }

    if (!is_directory) {
        fprintf(stderr, "Warning: missing %s directory '%s'. Run 'cup repair' to recreate it.\n", label, path);
        (*missing_count)++;
    }

    return CUP_OK;
}

CupError check_cup_structure(size_t *missing_count) {
    CupError err;
    char cup_root[MAX_PATH_LEN];
    char components_root[MAX_PATH_LEN];
    char tmp_root[MAX_PATH_LEN];
    char cache_root[MAX_PATH_LEN];
    char config_root[MAX_PATH_LEN];

    if (missing_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *missing_count = 0;

    err = get_cup_root_path(cup_root, sizeof(cup_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_config_root_path(config_root, sizeof(config_root));
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(cup_root, "cup root", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(components_root, "components", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(tmp_root, "temporary", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(cache_root, "cache", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(config_root, "config", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError ensure_cup_structure(void) {
    CupError err;
    char cup_root[MAX_PATH_LEN];
    char components_root[MAX_PATH_LEN];
    char tmp_root[MAX_PATH_LEN];
    char cache_root[MAX_PATH_LEN];
    char config_root[MAX_PATH_LEN];

    err = get_cup_root_path(cup_root, sizeof(cup_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = get_config_root_path(config_root, sizeof(config_root));
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(cup_root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(components_root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(tmp_root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(cache_root);
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(config_root);
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

    err = path_join(component_dir, sizeof(component_dir), components_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(tool_dir, sizeof(tool_dir), component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(host_dir, sizeof(host_dir), tool_dir, host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(target_dir, sizeof(target_dir), host_dir, target_platform);
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

    err = path_join(component_dir, sizeof(component_dir), cache_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(tool_dir, sizeof(tool_dir), component_dir, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(version_dir, sizeof(version_dir), tool_dir, version);
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

// PATH BUILDERS
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    char component_path[MAX_PATH_LEN];
    char tool_path[MAX_PATH_LEN];
    char host_path[MAX_PATH_LEN];
    char target_path[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_components_root_path(components_root, sizeof(components_root));
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(component_path, sizeof(component_path), components_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(tool_path, sizeof(tool_path), component_path, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(host_path, sizeof(host_path), tool_path, host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(target_path, sizeof(target_path), host_path, target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, target_path, version);
    return err;
}

static CupError build_tmp_path(char *buffer, size_t size, const char *operation, const char *component, const char *tool, const char *version, const char *suffix) {
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

static CupError build_safe_child_path(char *buffer, size_t size, const char *base_path, const char *relative_path) {
    CupError err;

    if (buffer == NULL || size == 0 || is_empty_string(base_path) || is_empty_string(relative_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (relative_path[0] == '/') {
        return CUP_ERR_INVALID_INPUT;
    }

    if (path_has_parent_ref(relative_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join(buffer, size, base_path, relative_path);
    return err;
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *version, const char *archive_name) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    char component_path[MAX_PATH_LEN];
    char tool_path[MAX_PATH_LEN];
    char cache_package_path[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(version) || is_empty_string(archive_name)) {
        fprintf(stderr, "Error: invalid cache archive path arguments.\n");
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cache_root_path(cache_root, sizeof(cache_root));
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(component_path, sizeof(component_path), cache_root, component);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(tool_path, sizeof(tool_path), component_path, tool);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(cache_package_path, sizeof(cache_package_path), tool_path, version);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, cache_package_path, archive_name);
    return err;
}

// FILE OPERATIONS
CupError copy_file(const char *source_path, const char *destination_path) {
    FILE *source;
    FILE *destination;
    char buffer[8192];
    size_t read_count;
    size_t write_count;

    if (is_empty_string(source_path) || is_empty_string(destination_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    source = fopen(source_path, "rb");
    if (source == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    destination = fopen(destination_path, "wb");
    if (destination == NULL) {
        fclose(source);
        return CUP_ERR_FILESYSTEM;
    }

    while ((read_count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        write_count = fwrite(buffer, 1, read_count, destination);
        if (write_count != read_count) {
            fclose(source);
            fclose(destination);
            system_remove_file(destination_path);
            return CUP_ERR_FILESYSTEM;
        }
    }

    if (ferror(source)) {
        fclose(source);
        fclose(destination);
        system_remove_file(destination_path);
        return CUP_ERR_FILESYSTEM;
    }

    if (fclose(source) != 0) {
        fclose(destination);
        system_remove_file(destination_path);
        return CUP_ERR_FILESYSTEM;
    }

    if (fclose(destination) != 0) {
        system_remove_file(destination_path);
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

// TMP / INSTALL VALIDATION
CupError create_tmp_dir(char *buffer, size_t size, const char *operation, const char *component, const char *tool, const char *version) {
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

static CupError require_path_type(const char *path, int want_directory) {
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

static CupError validate_child_path(const char *base_path, const char *relative_path, int want_directory) {
    CupError err;
    char path[MAX_PATH_LEN];

    err = build_safe_child_path(path, sizeof(path), base_path, relative_path);
    if (err != CUP_OK) {
        return err;
    }

    err = require_path_type(path, want_directory);
    return err;
}

static CupError validate_info_field(const char *info_path, const char *field, const char *expected_value) {
    CupError err;
    char actual_value[MAX_STATE_LINE_LEN];
    int found;

    if (is_empty_string(info_path) || is_empty_string(field) || is_empty_string(expected_value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = read_key_value_field(actual_value, sizeof(actual_value), info_path, field, &found);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not read package metadata '%s'.\n", info_path);
        return CUP_ERR_VALIDATION;
    }

    if (!found) {
        fprintf(stderr, "Error: package metadata is missing field '%s'.\n", field);
        return CUP_ERR_VALIDATION;
    }

    if (strcmp(actual_value, expected_value) != 0) {
        fprintf(stderr, "Error: package metadata field '%s' has value '%s', expected '%s'.\n",
            field, actual_value, expected_value);
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_info_file(const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
    CupError err;
    char info_path[MAX_PATH_LEN];

    if (is_empty_string(tmp_path) || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_safe_child_path(info_path, sizeof(info_path), tmp_path, "info.txt");
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(info_path, "package.component", component);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(info_path, "package.tool", tool);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(info_path, "package.version", version);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(info_path, "platform.host", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(info_path, "platform.target", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError validate_install(const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
    CupError err;

    if (is_empty_string(tmp_path) || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = require_path_type(tmp_path, 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: temporary install path '%s' is not a valid directory.\n", tmp_path);
        return CUP_ERR_VALIDATION;
    }

    err = validate_child_path(tmp_path, "info.txt", 0);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_info_file(tmp_path, component, tool, host_platform, target_platform, version);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_child_path(tmp_path, "bin", 1);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package does not contain a bin directory.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

// CLEANUP
CupError count_tmp_entries(size_t *count) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];
    int exists;

    if (count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *count = 0;

    err = get_tmp_root_path(tmp_root, sizeof(tmp_root));
    if (err != CUP_OK) {
        return err;
    }

    err = system_path_exists(tmp_root, &exists);
    if (err != CUP_OK) {
        return err;
    }

    if (!exists) {
        return CUP_OK;
    }

    err = system_walk_directory(tmp_root, count_visited_entry, count);
    if (err != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
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

// CONSISTENCY
CupError install_dir_exists(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *exists) {
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