/*
 * Constructs every canonical path under the fixed per-user .cup root and creates CUP assets,
 * runtime, cache, staging and recovery directories.
 */

#include "layout.h"

#include "filesystem.h"
#include "path.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <stdio.h>

/* Managed layout. */
static const char ROOT_DIRECTORY[] = ".cup";
static const char BIN_DIRECTORY[] = "bin";
static const char COMPONENTS_DIRECTORY[] = "components";
static const char STAGING_DIRECTORY[] = "staging";
static const char CACHE_DIRECTORY[] = "cache";
static const char RECOVERY_DIRECTORY[] = "recovery";
static const char CONFIG_DIRECTORY[] = "config";
static const char HELPERS_DIRECTORY[] = "helpers";

static const char STATE_FILENAME[] = "state.txt";
static const char LOCK_FILENAME[] = "cup.lock";
static const char TRANSACTION_FILENAME[] = "transaction.txt";

#if defined(_WIN32)
static const char BINARY_FILENAME[] = "cup.exe";
#else
static const char BINARY_FILENAME[] = "cup";
#endif

/* Managed directories. */
static const char *const RUNTIME_DIRS[] = {
    COMPONENTS_DIRECTORY, STAGING_DIRECTORY, CACHE_DIRECTORY};

static const char *const BOOTSTRAP_DIRS[] = {BIN_DIRECTORY, CONFIG_DIRECTORY, HELPERS_DIRECTORY};

/* Path helpers. */
typedef enum {
    PATH_CHAIN_ONLY,
    PATH_CHAIN_CREATE_DIRECTORIES
} PathChainMode;

/* Canonical root-relative path construction. */
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

static CupError build_path_chain(char *buffer,
                                 size_t size,
                                 const char *root,
                                 const char *const *parts,
                                 size_t count,
                                 PathChainMode mode) {
    CupError err;
    char current[MAX_PATH_LEN];
    char next[MAX_PATH_LEN];
    size_t i;

    if (buffer == NULL || size == 0 || text_is_empty(root) || parts == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = text_copy(current, sizeof(current), root);
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

        err = text_copy(current, sizeof(current), next);
        if (err != CUP_OK) {
            return err;
        }
    }

    return text_copy(buffer, size, current);
}

static CupError check_layout_directory(const char *path,
                                       const char *description,
                                       size_t *missing_count) {
    CupError err;
    int exists;
    int is_directory;

    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (!exists) {
        fprintf(stderr, "Issue: missing %s directory '%s'.\n", description, path);
        (*missing_count)++;
        return CUP_OK;
    }

    err = system_is_directory(path, &is_directory);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_directory) {
        fprintf(stderr, "Issue: %s path '%s' is not a directory.\n", description, path);
        (*missing_count)++;
    }

    return CUP_OK;
}

CupError layout_get_config_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, CONFIG_DIRECTORY);
}

/* Canonical paths. */
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

CupError layout_get_bin_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, BIN_DIRECTORY);
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, STAGING_DIRECTORY);
}

static CupError get_recovery_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, RECOVERY_DIRECTORY);
}

CupError layout_get_state_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, STATE_FILENAME);
}

CupError layout_get_lock_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, LOCK_FILENAME);
}

CupError layout_get_package_catalog_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_config_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_PACKAGES_FILENAME);
}

CupError layout_get_install_policy_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_config_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_INSTALL_POLICY_FILENAME);
}

CupError layout_get_preferences_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_config_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_PREFERENCES_FILENAME);
}

CupError layout_get_common_checksums_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = layout_get_config_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, CUP_COMMON_CHECKSUMS_FILENAME);
}

CupError layout_get_platform_checksums_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];
    char host[MAX_PLATFORM_LEN];
    char filename[MAX_PATH_LEN];

    err = layout_get_config_dir(directory, sizeof(directory));
    if (err != CUP_OK) {
        return err;
    }
    err = platform_get_host(host, sizeof(host));
    if (err != CUP_OK) {
        return err;
    }
    err = text_format(filename, sizeof(filename), "SHA256SUMS.%s", host);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, filename);
}

CupError layout_get_transaction_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, TRANSACTION_FILENAME);
}

CupError layout_get_cup_update_result_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_UPDATE_RESULT_FILENAME);
}

CupError layout_get_cup_update_helper_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];
    const char *filename = CUP_UPDATE_HELPER_FILENAME;
#if defined(_WIN32)
    char windows_name[MAX_IDENTIFIER_LEN];

    err = text_format(windows_name, sizeof(windows_name), "%s.exe", filename);
    if (err != CUP_OK) {
        return err;
    }
    filename = windows_name;
#endif

    err = build_root_path(directory, sizeof(directory), HELPERS_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, filename);
}

CupError layout_get_uninstall_marker_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_UNINSTALL_MARKER_FILENAME);
}

CupError layout_get_uninstall_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), HELPERS_DIRECTORY);
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

/* Package, cache, staging and recovery paths. */
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

CupError layout_build_cache_archive_path(char *buffer,
                                         size_t size,
                                         const PackageIdentity *identity,
                                         const char *format) {
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

    err = text_format(filename,
                      sizeof(filename),
                      "%s-%s-%s-%s.%s",
                      identity->tool,
                      identity->version,
                      identity->host_platform,
                      identity->target_platform,
                      format);
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, directory, filename);
}

/* Structure checks and creation. */
static CupError inspect_runtime_path(const char *path,
                                     SystemPathKind expected_kind,
                                     size_t *present_count,
                                     int *has_invalid_path) {
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

/* CUP assets/runtime inspection and creation. */
CupError layout_get_runtime_status(LayoutRuntimeStatus *status) {
    CupError err;
    char path[MAX_PATH_LEN];
    size_t present_count = 0;
    int has_invalid_path = 0;
    size_t i;

    if (status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *status = LAYOUT_RUNTIME_MISSING;

    for (i = 0; i < sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]); ++i) {
        err = build_root_path(path, sizeof(path), RUNTIME_DIRS[i]);
        if (err != CUP_OK) {
            return err;
        }

        err = inspect_runtime_path(path, SYSTEM_PATH_DIRECTORY, &present_count, &has_invalid_path);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = layout_get_state_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_runtime_path(path, SYSTEM_PATH_REGULAR_FILE, &present_count, &has_invalid_path);
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
    {
        SystemPathKind kind;
        int is_private = 0;

        err = system_get_path_kind(root, &kind);
        if (err != CUP_OK) {
            return err;
        }
        if (kind == SYSTEM_PATH_DIRECTORY) {
            err = system_directory_is_private(root, &is_private);
            if (err != CUP_OK) {
                return err;
            }
            if (!is_private) {
                fprintf(stderr, "Issue: cup root is not private to the current user.\n");
                (*missing_count)++;
            }
        }
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

    return system_make_private_directory(root);
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
    return ensure_directories(RUNTIME_DIRS, sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]));
}

CupError layout_ensure_config(void) {
    const char *const directory[] = {CONFIG_DIRECTORY};

    return ensure_directories(directory, 1);
}

CupError layout_ensure_cup_assets(void) {
    return ensure_directories(BOOTSTRAP_DIRS, sizeof(BOOTSTRAP_DIRS) / sizeof(BOOTSTRAP_DIRS[0]));
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

CupError layout_build_staging_prefix(char *buffer,
                                     size_t size,
                                     const char *operation,
                                     const PackageIdentity *identity) {
    if (buffer == NULL || size == 0 || identity == NULL || !path_is_safe_identifier(operation)) {
        return CUP_ERR_INVALID_INPUT;
    }

    return text_format(buffer,
                       size,
                       "%s-%s-%s-%s-%s-%s",
                       operation,
                       identity->component,
                       identity->tool,
                       identity->host_platform,
                       identity->target_platform,
                       identity->version);
}

CupError layout_create_staging_dir(char *buffer,
                                   size_t size,
                                   const char *operation,
                                   const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    char prefix[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_staging_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }
    err = layout_build_staging_prefix(prefix, sizeof(prefix), operation, identity);
    if (err != CUP_OK) {
        return err;
    }

    return system_create_temp_directory(root, prefix, buffer, size);
}

CupError layout_make_staging_path(char *buffer,
                                  size_t size,
                                  const char *operation,
                                  const PackageIdentity *identity) {
    CupError err;
    char root[MAX_PATH_LEN];
    char prefix[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_staging_dir(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }
    err = layout_build_staging_prefix(prefix, sizeof(prefix), operation, identity);
    if (err != CUP_OK) {
        return err;
    }

    return system_make_unique_temp_path(root, prefix, buffer, size);
}

CupError layout_create_recovery_dir(char *buffer, size_t size, const PackageIdentity *identity) {
    CupError err;
    char recovery_dir[MAX_PATH_LEN];
    char prefix[MAX_PATH_LEN];

    if (buffer == NULL || size == 0 || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = get_recovery_dir(recovery_dir, sizeof(recovery_dir));
    if (err != CUP_OK) {
        return err;
    }

    err = filesystem_ensure_directory(recovery_dir);
    if (err != CUP_OK) {
        return err;
    }

    err = text_format(prefix,
                      sizeof(prefix),
                      "invalid-%s-%s-%s-%s-%s",
                      identity->component,
                      identity->tool,
                      identity->host_platform,
                      identity->target_platform,
                      identity->version);
    if (err != CUP_OK) {
        return err;
    }

    return system_create_temp_directory(recovery_dir, prefix, buffer, size);
}
