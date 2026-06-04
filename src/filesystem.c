#include "filesystem.h"

#include "constants.h"
#include "interrupt.h"
#include "info.h"
#include "path.h"
#include "system.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define COPY_BUFFER_SIZE 8192

#define CUP_COMPONENTS_DIR "components"
#define CUP_TMP_DIR "tmp"
#define CUP_CACHE_DIR "cache"
#define CUP_CONFIG_DIR "config"
#define CUP_SCRIPTS_DIR "scripts"
#define CUP_STATE_FILE "state.txt"
#define CUP_MANIFEST_FILE "packages.cfg"
#define CUP_INFO_FILE "info.txt"

static const char *const CUP_BASE_DIRS[] = {
    CUP_COMPONENTS_DIR,
    CUP_TMP_DIR,
    CUP_CACHE_DIR,
    CUP_CONFIG_DIR,
    CUP_SCRIPTS_DIR
};

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

// RECURSIVE REMOVAL CALLBACKS
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

// CUP ROOT / PATH HELPERS
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

static CupError get_cup_child_path(char *buffer, size_t size, const char *child) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, root, child);
    return err;
}

CupError get_state_file_path(char *buffer, size_t size) {
    return get_cup_child_path(buffer, size, CUP_STATE_FILE);
}

CupError get_manifest_file_path(char *buffer, size_t size) {
    CupError err;
    char config_dir[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(config_dir, sizeof(config_dir), CUP_CONFIG_DIR);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, config_dir, CUP_MANIFEST_FILE);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError get_uninstall_script_path(char *buffer, size_t size) {
    CupError err;
    char scripts_dir[MAX_PATH_LEN];

#if defined(_WIN32)
    const char *script_name = "uninstall.ps1";
#else
    const char *script_name = "uninstall.sh";
#endif

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(scripts_dir, sizeof(scripts_dir), CUP_SCRIPTS_DIR);
    if (err != CUP_OK) {
        return err;
    }

    err = path_join(buffer, size, scripts_dir, script_name);
    return err;
}

// PATH CHAIN BUILDER
static CupError build_path_chain(char *buffer, size_t size, const char *root, const char *const *parts, size_t part_count, int create_dirs) {
    CupError err;
    char current[MAX_PATH_LEN];
    char next[MAX_PATH_LEN];
    size_t i;

    if (buffer == NULL || size == 0 || is_empty_string(root) || parts == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = checked_snprintf(current, sizeof(current), "%s", root);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < part_count; ++i) {
        if (is_empty_string(parts[i])) {
            return CUP_ERR_INVALID_INPUT;
        }

        err = path_join(next, sizeof(next), current, parts[i]);
        if (err != CUP_OK) {
            return err;
        }

        if (create_dirs) {
            err = create_directory(next);
            if (err != CUP_OK) {
                return err;
            }
        }

        err = checked_snprintf(current, sizeof(current), "%s", next);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = checked_snprintf(buffer, size, "%s", current);
    return err;
}

// STRUCTURE CHECK / ENSURE
static CupError check_required_directory(const char *path, const char *description, size_t *missing_count) {
    CupError err;
    int exists;
    int is_directory;

    if (is_empty_string(path) || is_empty_string(description) || missing_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: could not inspect %s directory '%s'.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    if (!exists) {
        fprintf(stderr, "Warning: missing %s directory '%s'. Run 'cup repair' to recreate it.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: could not inspect %s directory '%s'.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    if (!is_directory) {
        fprintf(stderr, "Warning: %s path '%s' exists but is not a directory.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    return CUP_OK;
}

CupError check_cup_structure(size_t *missing_count) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    size_t i;
    size_t count;

    if (missing_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *missing_count = 0;

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = check_required_directory(root, "cup root", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    count = sizeof(CUP_BASE_DIRS) / sizeof(CUP_BASE_DIRS[0]);

    for (i = 0; i < count; ++i) {
        err = get_cup_child_path(path, sizeof(path), CUP_BASE_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = check_required_directory(path, CUP_BASE_DIRS[i], missing_count);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

CupError ensure_cup_structure(void) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    size_t i;
    size_t count;

    err = get_cup_root_path(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = create_directory(root);
    if (err != CUP_OK) {
        return err;
    }

    count = sizeof(CUP_BASE_DIRS) / sizeof(CUP_BASE_DIRS[0]);

    for (i = 0; i < count; ++i) {
        err = get_cup_child_path(path, sizeof(path), CUP_BASE_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = create_directory(path);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

CupError ensure_component_base_dirs(const char *component, const char *tool, const char *host_platform, const char *target_platform) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    char target_path[MAX_PATH_LEN];
    const char *parts[4];

    if (is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(components_root, sizeof(components_root), CUP_COMPONENTS_DIR);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = component;
    parts[1] = tool;
    parts[2] = host_platform;
    parts[3] = target_platform;

    err = build_path_chain(target_path, sizeof(target_path), components_root, parts, 4, 1);
    return err;
}

CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *version) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    char package_path[MAX_PATH_LEN];
    const char *parts[3];

    if (is_empty_string(component) || is_empty_string(tool) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(cache_root, sizeof(cache_root), CUP_CACHE_DIR);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = component;
    parts[1] = tool;
    parts[2] = version;

    err = build_path_chain(package_path, sizeof(package_path), cache_root, parts, 3, 1);
    return err;
}

// CACHE / INSTALL PATH BUILDERS
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
    CupError err;
    char components_root[MAX_PATH_LEN];
    const char *parts[5];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(components_root, sizeof(components_root), CUP_COMPONENTS_DIR);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = component;
    parts[1] = tool;
    parts[2] = host_platform;
    parts[3] = target_platform;
    parts[4] = version;

    err = build_path_chain(buffer, size, components_root, parts, 5, 0);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *version, const char *archive_name) {
    CupError err;
    char cache_root[MAX_PATH_LEN];
    const char *parts[4];

    if (buffer == NULL || size == 0 || is_empty_string(component) || is_empty_string(tool) || 
        is_empty_string(version) || is_empty_string(archive_name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(cache_root, sizeof(cache_root), CUP_CACHE_DIR);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = component;
    parts[1] = tool;
    parts[2] = version;
    parts[3] = archive_name;

    err = build_path_chain(buffer, size, cache_root, parts, 4, 0);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// FILE COPY
CupError copy_file(const char *source_path, const char *destination_path) {
    FILE *source;
    FILE *destination;
    char buffer[COPY_BUFFER_SIZE];
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

// TMP HELPERS
static CupError build_tmp_path(char *buffer, size_t size, const char *operation, const char *component, const char *tool, const char *version, const char *suffix) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(operation) || is_empty_string(component) || 
        is_empty_string(tool) || is_empty_string(version) || is_empty_string(suffix)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_cup_child_path(tmp_root, sizeof(tmp_root), CUP_TMP_DIR);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(buffer, size, "%s/%s-%s-%s-%s-%s", tmp_root, operation, component, tool, version, suffix);
    return err;
}

CupError create_tmp_dir(char *buffer, size_t size, const char *operation, const char *component, const char *tool, const char *version) {
    CupError err;
    char suffix[MAX_NAME_LEN];

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

// INSTALL VALIDATION
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

    err = path_join_safe_relative(path, sizeof(path), base_path, relative_path);
    if (err != CUP_OK) {
        return err;
    }

    err = require_path_type(path, want_directory);
    return err;
}

static CupError validate_info_field(const PackageInfo *info, const char *field, const char *expected_value) {
    const char *actual_value;

    if (info == NULL || is_empty_string(field) || is_empty_string(expected_value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    actual_value = get_info_value(info, field);
    if (actual_value == NULL) {
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

static CupError validate_info_entries(const PackageInfo *info, const char *base_path) {
    CupError err;
    const PackageInfoField *field;
    size_t cursor;
    size_t entry_count;

    if (info == NULL || is_empty_string(base_path)) {
        return CUP_ERR_INVALID_INPUT;
    }

    cursor = 0;
    entry_count = 0;

    while ((field = next_info_field(info, "entry.", &cursor)) != NULL) {
        if (is_empty_string(field->value)) {
            fprintf(stderr, "Error: package metadata entry '%s' is empty.\n", field->key);
            return CUP_ERR_VALIDATION;
        }

        err = validate_child_path(base_path, field->value, 0);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: package metadata entry '%s' points to missing or invalid file '%s'.\n",
                field->key, field->value);
            return CUP_ERR_VALIDATION;
        }

        entry_count++;
    }

    if (entry_count == 0) {
        fprintf(stderr, "Error: package metadata does not declare any entry.* executable.\n");
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

static CupError validate_info_file(const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
    CupError err;
    PackageInfo info;
    char info_path[MAX_PATH_LEN];

    if (is_empty_string(tmp_path) || is_empty_string(component) || is_empty_string(tool) ||
        is_empty_string(host_platform) || is_empty_string(target_platform) || is_empty_string(version)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = path_join_safe_relative(info_path, sizeof(info_path), tmp_path, CUP_INFO_FILE);
    if (err != CUP_OK) {
        return err;
    }

    err = info_load(&info, info_path);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not read package metadata '%s'.\n", info_path);
        return CUP_ERR_VALIDATION;
    }

    err = validate_info_field(&info, "package.component", component);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(&info, "package.tool", tool);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(&info, "package.version", version);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(&info, "platform.host", host_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_field(&info, "platform.target", target_platform);
    if (err != CUP_OK) {
        return err;
    }

    err = validate_info_entries(&info, tmp_path);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

CupError validate_installation_metadata(const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version) {
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

    err = validate_child_path(tmp_path, CUP_INFO_FILE, 0);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: installed package metadata is missing or invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    err = validate_info_file(tmp_path, component, tool, host_platform, target_platform, version);
    if (err != CUP_OK) {
        return err;
    }

    return CUP_OK;
}

// TMP CLEANUP / PUBLIC CLEANUP
CupError count_tmp_entries(size_t *count) {
    CupError err;
    char tmp_root[MAX_PATH_LEN];
    int exists;

    if (count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *count = 0;

    err = get_cup_child_path(tmp_root, sizeof(tmp_root), CUP_TMP_DIR);
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
        fprintf(stderr, "Error: temporary path '%s' is not a directory.\n", tmp_path);
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

    err = get_cup_child_path(tmp_root, sizeof(tmp_root), CUP_TMP_DIR);
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