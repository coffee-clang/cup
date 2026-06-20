#include "layout.h"

#include "filesystem.h"
#include "path.h"
#include "system.h"
#include "util.h"

#include <stdio.h>

// MANAGED DIRECTORIES
static const char *const RUNTIME_DIRS[] = {
    CUP_COMPONENTS_DIR,
    CUP_TMP_DIR,
    CUP_CACHE_DIR
};

static const char *const BOOTSTRAP_DIRS[] = {
    CUP_BIN_DIR,
    CUP_CONFIG_DIR,
    CUP_SCRIPTS_DIR
};

// PATH HELPERS
static CupError build_root_path(char *buffer, size_t size, const char *child) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || is_empty_string(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, root, child);
}

static CupError build_path_chain(char *buffer, size_t size, const char *root,
    const char *const *parts, size_t count, int create) {
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

    for (i = 0; i < count; ++i) {
        if (!path_is_safe_segment(parts[i])) {
            return CUP_ERR_INVALID_INPUT;
        }

        err = path_join(next, sizeof(next), current, parts[i]);
        if (err != CUP_OK) {
            return err;
        }

        if (create) {
            err = filesystem_ensure_directory(next);
            if (err != CUP_OK) {
                return err;
            }
        }

        err = checked_snprintf(current, sizeof(current), "%s", next);
        if (err != CUP_OK) {
            return err;
        }
    }

    return checked_snprintf(buffer, size, "%s", current);
}

static CupError check_layout_directory(const char *path, const char *description, size_t *missing_count) {
    CupError err;
    int exists;
    int is_directory;

    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        fprintf(stderr, "Issue: missing %s directory '%s'.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK || !is_directory) {
        fprintf(stderr, "Issue: %s path '%s' is not a directory.\n", description, path);
        (*missing_count)++;
    }

    return CUP_OK;
}

// CANONICAL PATHS
CupError layout_get_root(char *buffer, size_t size) {
    CupError err;
    char home[MAX_PATH_LEN];

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(buffer, size, "%s/.cup", home);
}

CupError layout_get_components_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_COMPONENTS_DIR);
}

CupError layout_get_tmp_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_TMP_DIR);
}

CupError layout_get_state_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_STATE_FILE);
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_LOCK_FILE);
}

CupError layout_get_manifest_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), CUP_CONFIG_DIR);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_MANIFEST_FILE);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_tmp_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_TRANSACTION_FILE);
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];
#if defined(_WIN32)
    const char *name = "uninstall.ps1";
#else
    const char *name = "uninstall.sh";
#endif

    err = build_root_path(directory, sizeof(directory), CUP_SCRIPTS_DIR);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, name);
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];
#if defined(_WIN32)
    const char *name = "cup.exe";
#else
    const char *name = "cup";
#endif

    err = build_root_path(directory, sizeof(directory), CUP_BIN_DIR);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, name);
}

// PACKAGE PATHS
CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    const char *parts[5];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_components_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    parts[4] = identity->version;
    return build_path_chain(buffer, size, root, parts, 5, 0);
}

static CupError build_cache_dir(char *buffer, size_t size, const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    const char *parts[5];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_root_path(root, sizeof(root), CUP_CACHE_DIR);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    parts[4] = identity->version;
    return build_path_chain(buffer, size, root, parts, 5, 0);
}

CupError layout_build_cache_archive_path(char *buffer, size_t size,
    const PackageIdentity *identity, const char *format) {
    CupError err;
    char directory[MAX_PATH_LEN];
    char filename[MAX_PATH_LEN];

    if (identity == NULL || !path_is_safe_identifier(format)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_cache_dir(directory, sizeof(directory), identity);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(filename, sizeof(filename), "%s-%s-%s-%s.%s", identity->tool, identity->version,
        identity->host_platform, identity->target_platform, format);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, filename);
}

CupError layout_build_tmp_path(char *buffer, size_t size, const char *operation,
    const PackageIdentity *identity, const char *suffix) {
    CupError err;
    char root[MAX_PATH_LEN];
    char name[MAX_PATH_LEN];

    if (identity == NULL || !path_is_safe_identifier(operation) || !path_is_safe_identifier(suffix)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_tmp_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(name, sizeof(name), "%s-%s-%s-%s-%s-%s-%s", operation,
        identity->component, identity->tool, identity->host_platform, identity->target_platform,
        identity->version, suffix);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, root, name);
}

// STRUCTURE CHECKS AND CREATION
static CupError inspect_runtime_path(const char *path, int directory,
    size_t *present_count, int *invalid) {
    CupError err;
    int exists;
    int valid;

    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }

    (*present_count)++;
    err = directory ? system_is_directory(path, &valid) : system_is_regular_file(path, &valid);
    if (err != CUP_OK || !valid) {
        *invalid = 1;
    }

    return CUP_OK;
}

CupError layout_get_runtime_status(LayoutRuntimeStatus *status) {
    CupError err;
    char path[MAX_PATH_LEN];
    size_t present_count = 0;
    int invalid = 0;
    size_t i;

    if (status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]); ++i) {
        err = build_root_path(path, sizeof(path), RUNTIME_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = inspect_runtime_path(path, 1, &present_count, &invalid);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = layout_get_state_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_runtime_path(path, 0, &present_count, &invalid);
    if (err != CUP_OK) {
        return err;
    }

    if (present_count == 0) {
        *status = LAYOUT_RUNTIME_MISSING;
    } else if (!invalid && present_count == 4) {
        *status = LAYOUT_RUNTIME_READY;
    } else {
        *status = LAYOUT_RUNTIME_INCOMPLETE;
    }

    return CUP_OK;
}

CupError layout_check_runtime(size_t *missing_count) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    size_t i;

    if (missing_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *missing_count = 0;
    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = check_layout_directory(root, "cup root", missing_count);
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]); ++i) {
        err = build_root_path(path, sizeof(path), RUNTIME_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = check_layout_directory(path, RUNTIME_DIRS[i], missing_count);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

CupError layout_ensure_root(void) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return filesystem_ensure_directory(root);
}

static CupError ensure_directories(const char *const *directories, size_t count) {
    CupError err;
    char path[MAX_PATH_LEN];
    size_t i;

    err = layout_ensure_root();
    if (err != CUP_OK) {
        return err;
    }

    for (i = 0; i < count; ++i) {
        err = build_root_path(path, sizeof(path), directories[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = filesystem_ensure_directory(path);
        if (err != CUP_OK) {
            return err;
        }
    }

    return CUP_OK;
}

CupError layout_ensure_runtime(void) {
    return ensure_directories(RUNTIME_DIRS,
        sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]));
}

CupError layout_ensure_bootstrap(void) {
    return ensure_directories(BOOTSTRAP_DIRS,
        sizeof(BOOTSTRAP_DIRS) / sizeof(BOOTSTRAP_DIRS[0]));
}

CupError layout_ensure_package_parent(const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    const char *parts[4];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_components_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }
    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    return build_path_chain(path, sizeof(path), root, parts, 4, 1);
}

CupError layout_ensure_cache_parent(const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    const char *parts[5];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_root_path(root, sizeof(root), CUP_CACHE_DIR);
    if (err != CUP_OK) {
        return err;
    }
    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    parts[4] = identity->version;
    return build_path_chain(path, sizeof(path), root, parts, 5, 1);
}

CupError layout_create_tmp_dir(char *buffer, size_t size, const char *operation, const PackageIdentity *identity) {
    CupError err;
    char suffix[MAX_NAME_LEN];

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_process_id(suffix, sizeof(suffix));
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = layout_build_tmp_path(buffer, size, operation, identity, suffix);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = filesystem_remove_tree(buffer);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    err = filesystem_ensure_directory(buffer);
    if (err != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return CUP_OK;
}
