#include "layout.h"

#include "filesystem.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <stdio.h>

// MANAGED LAYOUT
static const char ROOT_DIRECTORY[] = ".cup";
static const char BIN_DIRECTORY[] = "bin";
static const char COMPONENTS_DIRECTORY[] = "components";
static const char TMP_DIRECTORY[] = "tmp";
static const char CACHE_DIRECTORY[] = "cache";
static const char CONFIG_DIRECTORY[] = "config";
static const char SCRIPTS_DIRECTORY[] = "scripts";

static const char STATE_FILENAME[] = "state.txt";
static const char LOCK_FILENAME[] = "cup.lock";
static const char TRANSACTION_FILENAME[] = "transaction.txt";

#if defined(_WIN32)
static const char BINARY_FILENAME[] = "cup.exe";
#else
static const char BINARY_FILENAME[] = "cup";
#endif

// MANAGED DIRECTORIES
static const char *const RUNTIME_DIRS[] = {
    COMPONENTS_DIRECTORY,
    TMP_DIRECTORY,
    CACHE_DIRECTORY
};

static const char *const BOOTSTRAP_DIRS[] = {
    BIN_DIRECTORY,
    CONFIG_DIRECTORY,
    SCRIPTS_DIRECTORY
};

// PATH HELPERS
typedef enum {
    PATH_CHAIN_ONLY,
    PATH_CHAIN_CREATE_DIRECTORIES
} PathChainMode;

static CupError build_root_path(char *buffer, size_t size, const char *child) {
    CupError err;
    char root[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || text_is_empty(child)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, root, child);
}

static CupError build_path_chain(char *buffer, size_t size, const char *root,
    const char *const *parts, size_t count, PathChainMode mode) {
    CupError err;
    char current[MAX_PATH_LEN];
    char next[MAX_PATH_LEN];
    size_t i;

    if (buffer == NULL || size == 0 || text_is_empty(root) || parts == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_format(current, sizeof(current), "%s", root);
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

        if (mode == PATH_CHAIN_CREATE_DIRECTORIES) {
            err = filesystem_ensure_directory(next);
            if (err != CUP_OK) {
                return err;
            }
        }

        err = text_format(current, sizeof(current), "%s", next);
        if (err != CUP_OK) {
            return err;
        }
    }

    return text_format(buffer, size, "%s", current);
}

static CupError check_layout_directory(const char *path,
    const char *description, size_t *missing_count) {
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

    return path_join(buffer, size, home, ROOT_DIRECTORY);
}

CupError layout_get_components_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, COMPONENTS_DIRECTORY);
}

CupError layout_get_tmp_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, TMP_DIRECTORY);
}

CupError layout_get_state_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, STATE_FILENAME);
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, LOCK_FILENAME);
}

CupError layout_get_manifest_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), CONFIG_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_MANIFEST_FILENAME);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_tmp_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, TRANSACTION_FILENAME);
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), SCRIPTS_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, CUP_UNINSTALL_FILENAME);
}

CupError layout_get_binary_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), BIN_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, BINARY_FILENAME);
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
    return build_path_chain(buffer, size, root, parts, 5, PATH_CHAIN_ONLY);
}

static CupError build_cache_dir(char *buffer, size_t size, const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    const char *parts[5];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_root_path(root, sizeof(root), CACHE_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }

    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    parts[4] = identity->version;
    return build_path_chain(buffer, size, root, parts, 5, PATH_CHAIN_ONLY);
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

    err = text_format(filename, sizeof(filename), "%s-%s-%s-%s.%s",
        identity->tool, identity->version,
        identity->host_platform, identity->target_platform, format);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, filename);
}

// STRUCTURE CHECKS AND CREATION
static CupError inspect_runtime_path(const char *path,
    SystemPathKind expected_kind, size_t *present_count, int *has_invalid_path) {
    SystemPathKind info;
    CupError err;

    err = system_get_path_kind(path, &info);
    if (err != CUP_OK || info == SYSTEM_PATH_MISSING) {
        return err;
    }

    (*present_count)++;
    if (info != expected_kind) {
        *has_invalid_path = 1;
    }

    return CUP_OK;
}

CupError layout_get_runtime_status(LayoutRuntimeStatus *status) {
    CupError err;
    char path[MAX_PATH_LEN];
    size_t present_count = 0;
    int has_invalid_path = 0;
    size_t i;

    if (status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]); ++i) {
        err = build_root_path(path, sizeof(path), RUNTIME_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = inspect_runtime_path(path, SYSTEM_PATH_DIRECTORY,
            &present_count, &has_invalid_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = layout_get_state_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_runtime_path(path, SYSTEM_PATH_REGULAR_FILE,
        &present_count, &has_invalid_path);
    if (err != CUP_OK) {
        return err;
    }

    if (present_count == 0) {
        *status = LAYOUT_RUNTIME_MISSING;
    } else if (!has_invalid_path &&
        present_count == sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]) + 1) {
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
    return build_path_chain(path, sizeof(path), root, parts, 4, PATH_CHAIN_CREATE_DIRECTORIES);
}

CupError layout_ensure_cache_parent(const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    const char *parts[5];

    if (identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_root_path(root, sizeof(root), CACHE_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }
    parts[0] = identity->component;
    parts[1] = identity->tool;
    parts[2] = identity->host_platform;
    parts[3] = identity->target_platform;
    parts[4] = identity->version;
    return build_path_chain(path, sizeof(path), root, parts, 5, PATH_CHAIN_CREATE_DIRECTORIES);
}

static CupError build_tmp_prefix(char *buffer, size_t size, const char *operation,
    const PackageIdentity *identity) {
    if (buffer == NULL || size == 0 || identity == NULL ||
        !path_is_safe_identifier(operation)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return text_format(buffer, size, "%s-%s-%s-%s-%s-%s", operation,
        identity->component, identity->tool, identity->host_platform,
        identity->target_platform, identity->version);
}

CupError layout_create_tmp_dir(char *buffer, size_t size, const char *operation,
    const PackageIdentity *identity) {
    char root[MAX_PATH_LEN];
    char prefix[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || identity == NULL ||
        layout_get_tmp_dir(root, sizeof(root)) != CUP_OK ||
        build_tmp_prefix(prefix, sizeof(prefix), operation, identity) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return system_create_temp_directory(root, prefix, buffer, size);
}

CupError layout_make_tmp_path(char *buffer, size_t size, const char *operation,
    const PackageIdentity *identity) {
    char root[MAX_PATH_LEN];
    char prefix[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || identity == NULL ||
        layout_get_tmp_dir(root, sizeof(root)) != CUP_OK ||
        build_tmp_prefix(prefix, sizeof(prefix), operation, identity) != CUP_OK) {
        return CUP_ERR_TEMPORARY;
    }

    return system_make_unique_temp_path(root, prefix, buffer, size);
}
