/*
 * Selects an owned per-user cup root and constructs every managed path below it. The primary
 * .cup name is preserved, while an unrelated pre-existing .cup directory causes deterministic
 * selection of .coffee-cup.
 */

#include "layout.h"

#include "filesystem.h"
#include "path.h"
#include "platform.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Fixed root-relative names. These constants define the on-disk contract used by every
 * path builder. */
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

/* Runtime directories that must exist after initialization. */
static const char *const RUNTIME_DIRS[] = {
    COMPONENTS_DIRECTORY, STAGING_DIRECTORY, CACHE_DIRECTORY};

static const char *const BOOTSTRAP_DIRS[] = {BIN_DIRECTORY, CONFIG_DIRECTORY, HELPERS_DIRECTORY};

typedef struct {
    char path[MAX_PATH_LEN];
    SystemPathIdentity identity;
    unsigned int depth;
} RootSnapshot;

static RootSnapshot root_snapshot;

/* Root-relative path composition. These helpers build strings only and never create
 * filesystem objects. */
typedef enum {
    PATH_CHAIN_ONLY,
    PATH_CHAIN_CREATE_DIRECTORIES
} PathChainMode;

typedef enum {
    ROOT_CANDIDATE_MISSING,
    ROOT_CANDIDATE_OWNED,
    ROOT_CANDIDATE_UNMARKED_CUP,
    ROOT_CANDIDATE_INVALID_MARKER,
    ROOT_CANDIDATE_FOREIGN
} RootCandidateStatus;

typedef enum {
    ROOT_MARKER_MISSING,
    ROOT_MARKER_VALID,
    ROOT_MARKER_INVALID
} RootMarkerStatus;

static CupError inspect_root_marker(const char *root, RootMarkerStatus *status) {
    PersistentFileSnapshot snapshot;
    SystemPathKind kind;
    char path[MAX_PATH_LEN];
    char expected[128];
    int written;
    CupError err;
    int missing;

    if (text_is_empty(root) || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *status = ROOT_MARKER_MISSING;
    written = snprintf(expected,
                       sizeof(expected),
                       "format=%d\nproduct=%s\nlayout=%d\n",
                       CUP_ROOT_MARKER_FORMAT,
                       CUP_ROOT_MARKER_PRODUCT,
                       CUP_ROOT_LAYOUT_FORMAT);
    if (written < 0 || (size_t)written >= sizeof(expected)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    err = path_join(path, sizeof(path), root, CUP_ROOT_MARKER_FILENAME);
    if (err == CUP_OK) {
        err = system_get_path_kind(path, &kind);
    }
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        *status = ROOT_MARKER_INVALID;
        return CUP_OK;
    }

    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, (size_t)written, &snapshot, &missing);
    if (err == CUP_ERR_BUFFER_TOO_SMALL) {
        *status = ROOT_MARKER_INVALID;
        return CUP_OK;
    }
    if (err != CUP_OK) {
        return err;
    }
    if (missing) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    *status = snapshot.size == (size_t)written &&
                      memcmp(snapshot.data, expected, (size_t)written) == 0
                  ? ROOT_MARKER_VALID
                  : ROOT_MARKER_INVALID;
    filesystem_snapshot_release(&snapshot);
    return CUP_OK;
}

static CupError candidate_path_kind(const char *root,
                                    const char *relative,
                                    SystemPathKind *kind,
                                    char *path,
                                    size_t path_size) {
    CupError err;

    if (text_is_empty(root) || text_is_empty(relative) || kind == NULL || path == NULL ||
        path_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = path_join(path, path_size, root, relative);
    return err == CUP_OK ? system_get_path_kind(path, kind) : err;
}

/*
 * Markerless roots are never adopted automatically. A canonical cup executable
 * is enough to treat the directory as cup-like and preserve it for explicit
 * diagnosis; weaker traces must not claim an unrelated directory named .cup.
 */
static CupError candidate_has_cup_binary(const char *root, int *has_binary) {
    char path[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    if (has_binary == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_binary = 0;
    err = candidate_path_kind(
        root, "bin/" CUP_BINARY_FILENAME, &kind, path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }
    *has_binary = kind != SYSTEM_PATH_MISSING;
    return CUP_OK;
}

static CupError classify_root_candidate(const char *root,
                                        RootCandidateStatus *status,
                                        SystemPathIdentity *identity) {
    CupError err;
    SystemPathIdentity initial_identity;
    SystemPathIdentity final_identity;
    int has_binary;
    RootMarkerStatus marker_status;

    if (text_is_empty(root) || status == NULL || identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(identity, 0, sizeof(*identity));
    memset(&initial_identity, 0, sizeof(initial_identity));
    memset(&final_identity, 0, sizeof(final_identity));

    err = system_get_path_identity(root, &initial_identity);
    if (err != CUP_OK) {
        return err;
    }
    if (!initial_identity.valid) {
        *status = ROOT_CANDIDATE_MISSING;
        return CUP_OK;
    }
    if (initial_identity.kind != SYSTEM_PATH_DIRECTORY) {
        *status = ROOT_CANDIDATE_FOREIGN;
        return CUP_OK;
    }

    err = inspect_root_marker(root, &marker_status);
    if (err != CUP_OK) {
        return err;
    }
    if (marker_status == ROOT_MARKER_VALID) {
        *status = ROOT_CANDIDATE_OWNED;
    } else if (marker_status == ROOT_MARKER_INVALID) {
        *status = ROOT_CANDIDATE_INVALID_MARKER;
    } else {
        err = candidate_has_cup_binary(root, &has_binary);
        if (err != CUP_OK) {
            return err;
        }
        *status = has_binary ? ROOT_CANDIDATE_UNMARKED_CUP : ROOT_CANDIDATE_FOREIGN;
    }

    /* Bind the classification to the same directory object. A root that changes while its
     * marker/traces are inspected is not a stable candidate for this command. */
    err = system_get_path_identity(root, &final_identity);
    if (err != CUP_OK) {
        return err;
    }
    if (!system_path_identity_equal(&initial_identity, &final_identity)) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    *identity = initial_identity;
    return CUP_OK;
}

static int root_candidate_is_recognized(RootCandidateStatus status) {
    return status == ROOT_CANDIDATE_OWNED;
}

static CupError inspect_root_candidates(char *primary,
                                        size_t primary_size,
                                        RootCandidateStatus *primary_status,
                                        SystemPathIdentity *primary_identity,
                                        char *fallback,
                                        size_t fallback_size,
                                        RootCandidateStatus *fallback_status,
                                        SystemPathIdentity *fallback_identity) {
    CupError err;
    char home[MAX_PATH_LEN];

    if (primary == NULL || primary_size == 0 || primary_status == NULL ||
        primary_identity == NULL || fallback == NULL || fallback_size == 0 ||
        fallback_status == NULL || fallback_identity == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err == CUP_OK) {
        err = path_join(primary, primary_size, home, CUP_PRIMARY_ROOT_DIRECTORY);
    }
    if (err == CUP_OK) {
        err = path_join(
            fallback, fallback_size, home, CUP_FALLBACK_ROOT_DIRECTORY);
    }
    if (err == CUP_OK) {
        err = classify_root_candidate(primary, primary_status, primary_identity);
    }
    if (err == CUP_OK) {
        err = classify_root_candidate(fallback, fallback_status, fallback_identity);
    }
    return err;
}

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

    if (buffer == NULL || size == 0 || text_is_empty(root) || parts == NULL ||
        (mode != PATH_CHAIN_ONLY && mode != PATH_CHAIN_CREATE_DIRECTORIES)) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Prove the complete chain and caller output capacity before creation can mutate any prefix. */
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
        err = text_copy(current, sizeof(current), next);
        if (err != CUP_OK) {
            return err;
        }
    }
    if (strlen(current) >= size) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    if (mode == PATH_CHAIN_ONLY) {
        return text_copy(buffer, size, current);
    }

    err = text_copy(current, sizeof(current), root);
    if (err != CUP_OK) {
        return err;
    }
    for (i = 0; i < count; ++i) {
        err = path_join(next, sizeof(next), current, parts[i]);
        if (err != CUP_OK) {
            return err;
        }
        err = filesystem_ensure_directory(next);
        if (err != CUP_OK) {
            return err;
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

/* Select one stable root without creating, adopting, or modifying either candidate. */
static CupError select_root(char *buffer, size_t size, SystemPathIdentity *selected_identity) {
    CupError err;
    char primary[MAX_PATH_LEN];
    char fallback[MAX_PATH_LEN];
    RootCandidateStatus primary_status;
    RootCandidateStatus fallback_status;
    SystemPathIdentity primary_identity;
    SystemPathIdentity fallback_identity;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = inspect_root_candidates(primary,
                                  sizeof(primary),
                                  &primary_status,
                                  &primary_identity,
                                  fallback,
                                  sizeof(fallback),
                                  &fallback_status,
                                  &fallback_identity);
    if (err != CUP_OK) {
        return err;
    }

    if (primary_status == ROOT_CANDIDATE_UNMARKED_CUP ||
        fallback_status == ROOT_CANDIDATE_UNMARKED_CUP) {
        const char *unmarked = primary_status == ROOT_CANDIDATE_UNMARKED_CUP
                                   ? primary
                                   : fallback;

        fprintf(stderr,
                "Error: an unmarked cup-like root was found at '%s'. cup cannot prove which "
                "development generation created it, so it was preserved and neither root "
                "candidate was selected or modified. Move the preserved directory to a backup "
                "path that is not '%s' or '%s', then run the current official installer. "
                "Recover only data accepted by the current formats; do not add root.txt "
                "manually.\n",
                unmarked,
                CUP_PRIMARY_ROOT_DIRECTORY,
                CUP_FALLBACK_ROOT_DIRECTORY);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (primary_status == ROOT_CANDIDATE_INVALID_MARKER ||
        fallback_status == ROOT_CANDIDATE_INVALID_MARKER) {
        const char *invalid = primary_status == ROOT_CANDIDATE_INVALID_MARKER ? primary
                                                                              : fallback;

        fprintf(stderr,
                "Error: cup root marker is invalid for the recognized root '%s'. "
                "The other root candidate was preserved and was not selected.\n",
                invalid);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (root_candidate_is_recognized(primary_status) &&
        root_candidate_is_recognized(fallback_status)) {
        fprintf(stderr,
                "Error: both cup root candidates are recognized: '%s' and '%s'.\n",
                primary,
                fallback);
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (root_candidate_is_recognized(primary_status)) {
        err = text_copy(buffer, size, primary);
        if (err == CUP_OK && selected_identity != NULL) {
            *selected_identity = primary_identity;
        }
        return err;
    }
    if (root_candidate_is_recognized(fallback_status)) {
        err = text_copy(buffer, size, fallback);
        if (err == CUP_OK && selected_identity != NULL) {
            *selected_identity = fallback_identity;
        }
        return err;
    }
    if (primary_status == ROOT_CANDIDATE_MISSING) {
        err = text_copy(buffer, size, primary);
        if (err == CUP_OK && selected_identity != NULL) {
            *selected_identity = primary_identity;
        }
        return err;
    }
    if (fallback_status == ROOT_CANDIDATE_MISSING) {
        err = text_copy(buffer, size, fallback);
        if (err == CUP_OK && selected_identity != NULL) {
            *selected_identity = fallback_identity;
        }
        return err;
    }

    fprintf(stderr,
            "Error: neither existing cup root candidate is recognized: '%s' or '%s'.\n",
            primary,
            fallback);
    return CUP_ERR_FILESYSTEM;
}

CupError layout_root_snapshot_begin(void) {
    CupError err;
    SystemPathIdentity identity;

    if (root_snapshot.depth != 0) {
        root_snapshot.depth++;
        return CUP_OK;
    }
    memset(&root_snapshot, 0, sizeof(root_snapshot));
    memset(&identity, 0, sizeof(identity));
    err = select_root(root_snapshot.path, sizeof(root_snapshot.path), &identity);
    if (err != CUP_OK) {
        return err;
    }
    root_snapshot.identity = identity;
    root_snapshot.depth = 1;
    return CUP_OK;
}

CupError layout_root_snapshot_validate(void) {
    SystemPathIdentity current;
    CupError err;

    if (root_snapshot.depth == 0) {
        return CUP_OK;
    }
    err = system_get_path_identity(root_snapshot.path, &current);
    if (err != CUP_OK) {
        return err;
    }
    if (!current.valid || current.kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (!root_snapshot.identity.valid) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    return system_path_identity_equal(&root_snapshot.identity, &current)
               ? CUP_OK
               : CUP_ERR_INCONSISTENT_STATE;
}

void layout_root_snapshot_end(void) {
    if (root_snapshot.depth == 0) {
        return;
    }
    root_snapshot.depth--;
    if (root_snapshot.depth == 0) {
        memset(&root_snapshot, 0, sizeof(root_snapshot));
    }
}

CupError layout_get_root(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (root_snapshot.depth != 0) {
        return text_copy(buffer, size, root_snapshot.path);
    }
    return select_root(buffer, size, NULL);
}

CupError layout_check_root_candidates(size_t *issue_count) {
    CupError err;
    char primary[MAX_PATH_LEN];
    char fallback[MAX_PATH_LEN];
    RootCandidateStatus primary_status;
    RootCandidateStatus fallback_status;

    if (issue_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *issue_count = 0;

    {
        SystemPathIdentity primary_identity;
        SystemPathIdentity fallback_identity;

        err = inspect_root_candidates(primary,
                                      sizeof(primary),
                                      &primary_status,
                                      &primary_identity,
                                      fallback,
                                      sizeof(fallback),
                                      &fallback_status,
                                      &fallback_identity);
    }
    if (err != CUP_OK) {
        return err;
    }

    if (primary_status == ROOT_CANDIDATE_UNMARKED_CUP) {
        printf("Issue: unmarked cup-like root '%s' cannot be adopted automatically.\n", primary);
        (*issue_count)++;
    }
    if (fallback_status == ROOT_CANDIDATE_UNMARKED_CUP) {
        printf("Issue: unmarked cup-like root '%s' cannot be adopted automatically.\n", fallback);
        (*issue_count)++;
    }
    if (primary_status == ROOT_CANDIDATE_INVALID_MARKER) {
        printf("Issue: cup root marker is invalid for recognized root '%s'.\n", primary);
        (*issue_count)++;
    }
    if (fallback_status == ROOT_CANDIDATE_INVALID_MARKER) {
        printf("Issue: cup root marker is invalid for recognized root '%s'.\n", fallback);
        (*issue_count)++;
    }
    if (*issue_count != 0) {
        printf("Info: neither cup root candidate was selected or modified.\n");
        printf("Recovery: move each reported cup-like/invalid root to a backup path outside "
               "'%s' and '%s', then run the current official installer. Recover only data "
               "accepted by current formats and do not create root.txt manually.\n",
               CUP_PRIMARY_ROOT_DIRECTORY,
               CUP_FALLBACK_ROOT_DIRECTORY);
        printf("Info: 'cup repair' cannot resolve root ownership while a reported candidate "
               "blocks root selection.\n");
        return CUP_OK;
    }

    if (root_candidate_is_recognized(primary_status) &&
        root_candidate_is_recognized(fallback_status)) {
        printf("Issue: both cup root candidates are recognized: '%s' and '%s'.\n",
               primary,
               fallback);
        (*issue_count)++;
    } else if (!root_candidate_is_recognized(primary_status) &&
               !root_candidate_is_recognized(fallback_status) &&
               primary_status != ROOT_CANDIDATE_MISSING &&
               fallback_status != ROOT_CANDIDATE_MISSING) {
        printf("Issue: neither existing cup root candidate is recognized: '%s' or '%s'.\n",
               primary,
               fallback);
        (*issue_count)++;
    }
    return CUP_OK;
}

/* Canonical singleton paths for state, journals, configuration and official cup assets. */
CupError layout_get_components_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, COMPONENTS_DIRECTORY);
}

CupError layout_get_bin_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, BIN_DIRECTORY);
}

CupError layout_get_staging_dir(char *buffer, size_t size) {
    return build_root_path(buffer, size, STAGING_DIRECTORY);
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

CupError layout_get_cup_update_helper_path(char *buffer, size_t size) {
    CupError err;
    char directory[MAX_PATH_LEN];

    err = build_root_path(directory, sizeof(directory), HELPERS_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, directory, CUP_UPDATE_HELPER_FILENAME);
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

    return path_join(buffer, size, directory, CUP_BINARY_FILENAME);
}

/* Paths derived from one validated package identity for packages, cache, staging and recovery. */
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

/* Runtime structure validation and private-directory creation. */
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

/* Distinguish an absent runtime from a partially initialized one before commands decide
 * whether creation is allowed. */
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

    if (!has_invalid_path &&
        present_count == sizeof(RUNTIME_DIRS) / sizeof(RUNTIME_DIRS[0]) + 1) {
        char root[MAX_PATH_LEN];
        int is_private = 0;

        err = layout_get_root(root, sizeof(root));
        if (err == CUP_OK) {
            err = system_directory_is_private(root, &is_private);
        }
        if (err != CUP_OK) {
            return err;
        }
        if (!is_private) {
            has_invalid_path = 1;
        }
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

    {
        SystemPathKind root_kind;
        RootMarkerStatus marker_status = ROOT_MARKER_MISSING;

        err = system_get_path_kind(root, &root_kind);
        if (err != CUP_OK) {
            return err;
        }
        if (root_kind == SYSTEM_PATH_DIRECTORY) {
            err = build_root_path(path, sizeof(path), CUP_ROOT_MARKER_FILENAME);
            if (err == CUP_OK) {
                err = inspect_root_marker(root, &marker_status);
            }
            if (err != CUP_OK) {
                return err;
            }
            if (marker_status != ROOT_MARKER_VALID) {
                fprintf(stderr, "Issue: cup root marker is missing or invalid: '%s'.\n", path);
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

static CupError write_root_marker(FILE *file, const void *value) {
    (void)value;

    if (file == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    return fprintf(file,
                   "format=%d\nproduct=%s\nlayout=%d\n",
                   CUP_ROOT_MARKER_FORMAT,
                   CUP_ROOT_MARKER_PRODUCT,
                   CUP_ROOT_LAYOUT_FORMAT) < 0
               ? CUP_ERR_FILESYSTEM
               : CUP_OK;
}

static CupError ensure_root_marker(const char *root) {
    CupError err;
    char marker[MAX_PATH_LEN];
    RootMarkerStatus marker_status;

    err = inspect_root_marker(root, &marker_status);
    if (err != CUP_OK || marker_status == ROOT_MARKER_VALID) {
        return err;
    }
    err = path_join(marker, sizeof(marker), root, CUP_ROOT_MARKER_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    if (marker_status == ROOT_MARKER_INVALID) {
        fprintf(stderr, "Error: cup root marker is invalid: '%s'.\n", marker);
        return CUP_ERR_VALIDATION;
    }

    return filesystem_publish_new_file(
        root, "root-marker", marker, 0, write_root_marker, NULL);
}

CupError layout_ensure_root(void) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    char root[MAX_PATH_LEN];
    int local_snapshot = 0;
    int root_created = 0;

    /* Root creation must always be tied to one selected path and its missing/existing identity. */
    if (root_snapshot.depth == 0) {
        err = layout_root_snapshot_begin();
        if (err != CUP_OK) {
            return err;
        }
        local_snapshot = 1;
    }

    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        goto done;
    }

    if (!root_snapshot.identity.valid) {
        err = system_create_private_directory(root, &commit_state);
        if (err == CUP_OK && commit_state != SYSTEM_COMMIT_DURABLE) {
            err = CUP_ERR_COMMIT;
        }
        if (err == CUP_OK) {
            root_created = 1;
            err = system_get_path_identity(root, &root_snapshot.identity);
            if (err == CUP_OK &&
                (!root_snapshot.identity.valid ||
                 root_snapshot.identity.kind != SYSTEM_PATH_DIRECTORY)) {
                err = CUP_ERR_INCONSISTENT_STATE;
            }
        }
        if (err != CUP_OK) {
            /* The exclusive create may already be visible. Do not leave an unmarked candidate
             * that a later process can no longer prove belongs to cup. */
            if (commit_state != SYSTEM_COMMIT_NOT_APPLIED) {
                CupError rollback_err = system_remove_directory(root);

                if (rollback_err == CUP_OK) {
                    rollback_err = system_sync_parent_directory(root);
                }
                if (rollback_err != CUP_OK) {
                    err = CUP_ERR_ROLLBACK;
                }
            }
            memset(&root_snapshot.identity, 0, sizeof(root_snapshot.identity));
            goto done;
        }
    } else {
        /* Do not change permissions on a path that no longer names the directory selected for
         * this command. The final validation below still closes the normal post-mutation check. */
        err = layout_root_snapshot_validate();
        if (err == CUP_OK) {
            err = system_make_private_directory(root);
        }
    }
    if (err == CUP_OK) {
        err = ensure_root_marker(root);
    }
    if (err != CUP_OK && root_created) {
        RootMarkerStatus marker_status = ROOT_MARKER_MISSING;
        CupError marker_err = inspect_root_marker(root, &marker_status);

        /* A complete marker may already be visible when its parent sync failed. Keep that
         * ownership proof and surface the original commit error. Otherwise roll an empty new
         * root back rather than stranding a markerless candidate. */
        if (marker_err != CUP_OK || marker_status != ROOT_MARKER_VALID) {
            CupError rollback_err = marker_status == ROOT_MARKER_MISSING
                                        ? system_remove_directory(root)
                                        : CUP_ERR_ROLLBACK;

            if (rollback_err == CUP_OK) {
                rollback_err = system_sync_parent_directory(root);
            }
            if (rollback_err != CUP_OK) {
                err = CUP_ERR_ROLLBACK;
            } else {
                memset(&root_snapshot.identity, 0, sizeof(root_snapshot.identity));
            }
        }
    }
    if (err == CUP_OK) {
        err = layout_root_snapshot_validate();
    }

done:
    if (local_snapshot) {
        layout_root_snapshot_end();
    }
    return err;
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
    if (buffer == NULL || size == 0 || identity == NULL || !path_is_safe_identifier(operation) ||
        !path_is_safe_segment(identity->component) || !path_is_safe_segment(identity->tool) ||
        !path_is_safe_segment(identity->host_platform) ||
        !path_is_safe_segment(identity->target_platform) ||
        !path_is_safe_segment(identity->version)) {
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

    err = layout_build_staging_prefix(prefix, sizeof(prefix), "invalid", identity);
    if (err != CUP_OK) {
        return err;
    }
    err = build_root_path(recovery_dir, sizeof(recovery_dir), RECOVERY_DIRECTORY);
    if (err != CUP_OK) {
        return err;
    }
    err = filesystem_ensure_directory(recovery_dir);
    if (err != CUP_OK) {
        return err;
    }

    return system_create_temp_directory(recovery_dir, prefix, buffer, size);
}
