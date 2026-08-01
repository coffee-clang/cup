/*
 * Selects an owned per-user cup root and constructs every managed path below it. The primary
 * .cup name is preserved, while an unrelated pre-existing .cup directory causes deterministic
 * selection of .coffee-cup.
 */

#include "layout.h"

#include "checksum.h"
#include "filesystem.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "path.h"
#include "platform.h"
#include "state.h"
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

/* Root-relative path composition. These helpers build strings only and never create
 * filesystem objects. */
typedef enum {
    PATH_CHAIN_ONLY,
    PATH_CHAIN_CREATE_DIRECTORIES
} PathChainMode;

typedef enum {
    ROOT_CANDIDATE_MISSING,
    ROOT_CANDIDATE_OWNED,
    ROOT_CANDIDATE_LEGACY,
    ROOT_CANDIDATE_INVALID_MARKER,
    ROOT_CANDIDATE_DAMAGED,
    ROOT_CANDIDATE_FOREIGN
} RootCandidateStatus;

static CupError build_home_root(char *buffer,
                                size_t size,
                                const char *home,
                                const char *directory) {
    if (buffer == NULL || size == 0 || text_is_empty(home) || text_is_empty(directory)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return path_join(buffer, size, home, directory);
}

static CupError build_candidate_path(char *buffer,
                                     size_t size,
                                     const char *root,
                                     const char *relative) {
    if (buffer == NULL || size == 0 || text_is_empty(root) || text_is_empty(relative)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return path_join(buffer, size, root, relative);
}

static int marker_line_matches(char *line, const char *expected) {
    size_t length;

    if (line == NULL || expected == NULL) {
        return 0;
    }
    length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
    return strcmp(line, expected) == 0;
}

static CupError root_marker_is_valid(const char *root, int *valid) {
    CupError err;
    SystemPathKind kind;
    char path[MAX_PATH_LEN];
    char first[32];
    char second[64];
    char third[32];
    char extra[2];
    FILE *file;

    if (text_is_empty(root) || valid == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *valid = 0;

    err = build_candidate_path(path, sizeof(path), root, CUP_ROOT_MARKER_FILENAME);
    if (err != CUP_OK) {
        return err;
    }
    err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind == SYSTEM_PATH_MISSING) {
        return err;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_OK;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }
    first[0] = '\0';
    second[0] = '\0';
    third[0] = '\0';
    extra[0] = '\0';
    if (fgets(first, sizeof(first), file) != NULL &&
        fgets(second, sizeof(second), file) != NULL &&
        fgets(third, sizeof(third), file) != NULL &&
        fgets(extra, sizeof(extra), file) == NULL && !ferror(file) &&
        marker_line_matches(first, "format=1") &&
        marker_line_matches(second, "product=" CUP_ROOT_MARKER_PRODUCT) &&
        marker_line_matches(third, "layout=1")) {
        *valid = 1;
    }
    if (fclose(file) != 0) {
        return CUP_ERR_FILESYSTEM;
    }
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
    err = build_candidate_path(path, path_size, root, relative);
    return err == CUP_OK ? system_get_path_kind(path, kind) : err;
}

static CupError candidate_has_cup_traces(const char *root, int *has_traces) {
    static const char *const TRACE_PATHS[] = {
        "bin/" CUP_BINARY_FILENAME,
        "helpers/" CUP_UPDATE_HELPER_FILENAME,
        "helpers/" CUP_UNINSTALL_FILENAME,
        "config/" CUP_COMMON_CHECKSUMS_FILENAME,
        STATE_FILENAME};
    char path[MAX_PATH_LEN];
    SystemPathKind kind;
    size_t i;
    CupError err;

    if (has_traces == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_traces = 0;
    for (i = 0; i < sizeof(TRACE_PATHS) / sizeof(TRACE_PATHS[0]); ++i) {
        err = candidate_path_kind(root, TRACE_PATHS[i], &kind, path, sizeof(path));
        if (err != CUP_OK) {
            return err;
        }
        if (kind != SYSTEM_PATH_MISSING) {
            *has_traces = 1;
            return CUP_OK;
        }
    }
    return CUP_OK;
}

/*
 * A markerless directory is treated as a damaged legacy CUP root only when the
 * canonical CUP executable is present. Other familiar-looking files are useful
 * evidence beside an invalid marker, but they must not turn an unrelated
 * markerless directory into CUP-owned data.
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

static CupError candidate_platform_asset_name(char *buffer, size_t size) {
    char host[MAX_PLATFORM_LEN];
    CupError err = platform_get_host(host, sizeof(host));

    if (err != CUP_OK) {
        return err;
    }
    return strcmp(host, "windows-x64") == 0
               ? text_format(buffer, size, "cup-%s.exe", host)
               : text_format(buffer, size, "cup-%s", host);
}

static CupError candidate_platform_checksums_path(const char *root,
                                                  char *buffer,
                                                  size_t size) {
    char host[MAX_PLATFORM_LEN];
    char filename[MAX_PATH_LEN];
    char config[MAX_PATH_LEN];
    CupError err = platform_get_host(host, sizeof(host));

    if (err == CUP_OK) {
        err = text_format(filename, sizeof(filename), "SHA256SUMS.%s", host);
    }
    if (err == CUP_OK) {
        err = build_candidate_path(config, sizeof(config), root, CONFIG_DIRECTORY);
    }
    return err == CUP_OK ? path_join(buffer, size, config, filename) : err;
}

static CupError candidate_regular_file(const char *path, int executable) {
    SystemPathKind kind;
    int is_executable;
    CupError err = system_get_path_kind(path, &kind);

    if (err != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) {
        return err == CUP_OK ? CUP_ERR_VALIDATION : err;
    }
    if (!executable) {
        return CUP_OK;
    }
    err = system_is_executable(path, &is_executable);
    return err == CUP_OK && is_executable ? CUP_OK : (err == CUP_OK ? CUP_ERR_VALIDATION : err);
}

static CupError candidate_directory(const char *root, const char *relative) {
    char path[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err = candidate_path_kind(root, relative, &kind, path, sizeof(path));

    if (err != CUP_OK) {
        return err;
    }
    return kind == SYSTEM_PATH_DIRECTORY ? CUP_OK : CUP_ERR_VALIDATION;
}

/*
 * A markerless root is adopted only when its complete installed CUP generation is internally
 * coherent. Co-located checksum files are not a cryptographic signature, but the exact asset set,
 * executable/helper identity and strict parsers form a strong legacy ownership boundary without
 * running an untrusted executable.
 */
static CupError legacy_root_is_recognized(const char *root, int *recognized) {
    PackageCatalog catalog;
    InstallPolicy policy;
    CupState state;
    StateFileStatus state_status;
    char binary[MAX_PATH_LEN];
    char helper[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char catalog_path[MAX_PATH_LEN];
    char policy_path[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char state_path[MAX_PATH_LEN];
    char binary_asset[MAX_IDENTIFIER_LEN];
    char binary_hash[SHA256_HEX_LENGTH + 1];
    char helper_hash[SHA256_HEX_LENGTH + 1];
    const char *platform_assets[3];
    SystemPathKind state_kind;
    int matches = 0;
    CupError err;

    if (recognized == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *recognized = 0;

    err = build_candidate_path(binary, sizeof(binary), root, "bin/" CUP_BINARY_FILENAME);
    if (err == CUP_OK) {
        err = build_candidate_path(
            helper, sizeof(helper), root, "helpers/" CUP_UPDATE_HELPER_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_candidate_path(
            uninstall, sizeof(uninstall), root, "helpers/" CUP_UNINSTALL_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_candidate_path(
            catalog_path, sizeof(catalog_path), root, "config/" CUP_PACKAGES_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_candidate_path(
            policy_path, sizeof(policy_path), root, "config/" CUP_INSTALL_POLICY_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_candidate_path(common_checksums,
                                   sizeof(common_checksums),
                                   root,
                                   "config/" CUP_COMMON_CHECKSUMS_FILENAME);
    }
    if (err == CUP_OK) {
        err = candidate_platform_checksums_path(root, platform_checksums, sizeof(platform_checksums));
    }
    if (err == CUP_OK) {
        err = build_candidate_path(state_path, sizeof(state_path), root, STATE_FILENAME);
    }
    if (err == CUP_OK) {
        err = candidate_platform_asset_name(binary_asset, sizeof(binary_asset));
    }
    if (err != CUP_OK) {
        return err;
    }

    if (candidate_directory(root, BIN_DIRECTORY) != CUP_OK ||
        candidate_directory(root, COMPONENTS_DIRECTORY) != CUP_OK ||
        candidate_directory(root, STAGING_DIRECTORY) != CUP_OK ||
        candidate_directory(root, CACHE_DIRECTORY) != CUP_OK ||
        candidate_directory(root, CONFIG_DIRECTORY) != CUP_OK ||
        candidate_directory(root, HELPERS_DIRECTORY) != CUP_OK ||
        candidate_regular_file(binary, 1) != CUP_OK ||
        candidate_regular_file(helper, 1) != CUP_OK ||
        candidate_regular_file(uninstall, CUP_UNINSTALL_EXECUTABLE) != CUP_OK ||
        candidate_regular_file(catalog_path, 0) != CUP_OK ||
        candidate_regular_file(policy_path, 0) != CUP_OK ||
        candidate_regular_file(common_checksums, 0) != CUP_OK ||
        candidate_regular_file(platform_checksums, 0) != CUP_OK) {
        return CUP_OK;
    }

    platform_assets[0] = binary_asset;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    platform_assets[2] = CUP_RELEASE_METADATA_FILENAME;
    if (checksum_validate_assets(common_checksums,
                                 CUP_COMMON_CHECKSUM_ASSETS,
                                 CUP_COMMON_CHECKSUM_ASSET_COUNT) != CUP_OK ||
        checksum_validate_assets(platform_checksums,
                                 platform_assets,
                                 sizeof(platform_assets) / sizeof(platform_assets[0])) != CUP_OK ||
        checksum_verify_file(platform_checksums, binary_asset, binary, &matches) != CUP_OK ||
        !matches ||
        checksum_verify_file(
            platform_checksums, CUP_UNINSTALL_FILENAME, uninstall, &matches) != CUP_OK ||
        !matches ||
        checksum_verify_file(
            common_checksums, CUP_PACKAGES_FILENAME, catalog_path, &matches) != CUP_OK ||
        !matches ||
        checksum_verify_file(
            common_checksums, CUP_INSTALL_POLICY_FILENAME, policy_path, &matches) != CUP_OK ||
        !matches) {
        return CUP_OK;
    }

    if (checksum_sha256_file(binary, binary_hash, sizeof(binary_hash)) != CUP_OK ||
        checksum_sha256_file(helper, helper_hash, sizeof(helper_hash)) != CUP_OK ||
        strcmp(binary_hash, helper_hash) != 0) {
        return CUP_OK;
    }

    package_catalog_init(&catalog);
    err = package_catalog_load_path(
        &catalog, catalog_path, PACKAGE_CATALOG_SOURCE_INSTALLED);
    package_catalog_free(&catalog);
    if (err != CUP_OK) {
        return err == CUP_ERR_CATALOG ? CUP_OK : err;
    }

    install_policy_init(&policy);
    err = install_policy_load_path(&policy, policy_path, INSTALL_POLICY_SOURCE_INSTALLED);
    if (err != CUP_OK) {
        return err == CUP_ERR_VALIDATION ? CUP_OK : err;
    }

    err = system_get_path_kind(state_path, &state_kind);
    if (err != CUP_OK) {
        return err;
    }
    if (state_kind != SYSTEM_PATH_MISSING && state_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_OK;
    }
    err = state_load_path(&state, &state_status, state_path);
    if (err != CUP_OK) {
        return err == CUP_ERR_STATE_LOAD ? CUP_OK : err;
    }
    if (state_status == STATE_FILE_LOADED && state_validate(&state) != CUP_OK) {
        return CUP_OK;
    }

    *recognized = 1;
    return CUP_OK;
}

static CupError classify_root_candidate(const char *root, RootCandidateStatus *status) {
    CupError err;
    SystemPathKind kind;
    int recognized;
    int has_binary;
    int has_traces;
    int marker_valid;

    if (text_is_empty(root) || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_path_kind(root, &kind);
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        *status = ROOT_CANDIDATE_MISSING;
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_DIRECTORY) {
        *status = ROOT_CANDIDATE_FOREIGN;
        return CUP_OK;
    }

    err = root_marker_is_valid(root, &marker_valid);
    if (err != CUP_OK) {
        return err;
    }
    if (marker_valid) {
        *status = ROOT_CANDIDATE_OWNED;
        return CUP_OK;
    }

    {
        char marker[MAX_PATH_LEN];
        SystemPathKind marker_kind;

        err = build_candidate_path(marker, sizeof(marker), root, CUP_ROOT_MARKER_FILENAME);
        if (err == CUP_OK) {
            err = system_get_path_kind(marker, &marker_kind);
        }
        if (err != CUP_OK) {
            return err;
        }
        if (marker_kind != SYSTEM_PATH_MISSING) {
            err = candidate_has_cup_traces(root, &has_traces);
            if (err != CUP_OK) {
                return err;
            }
            *status = has_traces ? ROOT_CANDIDATE_INVALID_MARKER : ROOT_CANDIDATE_FOREIGN;
            return CUP_OK;
        }
    }

    err = legacy_root_is_recognized(root, &recognized);
    if (err != CUP_OK) {
        return err;
    }
    if (recognized) {
        *status = ROOT_CANDIDATE_LEGACY;
        return CUP_OK;
    }
    err = candidate_has_cup_binary(root, &has_binary);
    if (err != CUP_OK) {
        return err;
    }
    *status = has_binary ? ROOT_CANDIDATE_DAMAGED : ROOT_CANDIDATE_FOREIGN;
    return CUP_OK;
}

static int root_candidate_is_recognized(RootCandidateStatus status) {
    return status == ROOT_CANDIDATE_OWNED || status == ROOT_CANDIDATE_LEGACY;
}

static CupError inspect_root_candidates(char *primary,
                                        size_t primary_size,
                                        RootCandidateStatus *primary_status,
                                        char *alternative,
                                        size_t alternative_size,
                                        RootCandidateStatus *alternative_status) {
    CupError err;
    char home[MAX_PATH_LEN];

    if (primary == NULL || primary_size == 0 || primary_status == NULL || alternative == NULL ||
        alternative_size == 0 || alternative_status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = system_get_home_dir(home, sizeof(home));
    if (err == CUP_OK) {
        err = build_home_root(primary, primary_size, home, CUP_PRIMARY_ROOT_DIRECTORY);
    }
    if (err == CUP_OK) {
        err = build_home_root(
            alternative, alternative_size, home, CUP_ALTERNATIVE_ROOT_DIRECTORY);
    }
    if (err == CUP_OK) {
        err = classify_root_candidate(primary, primary_status);
    }
    if (err == CUP_OK) {
        err = classify_root_candidate(alternative, alternative_status);
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

/* Select one stable root without creating, adopting, or modifying either candidate. */
CupError layout_get_root(char *buffer, size_t size) {
    CupError err;
    char primary[MAX_PATH_LEN];
    char alternative[MAX_PATH_LEN];
    RootCandidateStatus primary_status;
    RootCandidateStatus alternative_status;

    if (buffer == NULL || size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = inspect_root_candidates(primary,
                                  sizeof(primary),
                                  &primary_status,
                                  alternative,
                                  sizeof(alternative),
                                  &alternative_status);
    if (err != CUP_OK) {
        return err;
    }

    if (primary_status == ROOT_CANDIDATE_DAMAGED ||
        alternative_status == ROOT_CANDIDATE_DAMAGED) {
        const char *damaged = primary_status == ROOT_CANDIDATE_DAMAGED ? primary : alternative;

        fprintf(stderr,
                "Error: a probable legacy cup root was found at '%s', but its installed "
                "generation could not be verified. Neither root candidate was selected or "
                "modified. Run 'cup doctor' or the official installer.\n",
                damaged);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (primary_status == ROOT_CANDIDATE_INVALID_MARKER ||
        alternative_status == ROOT_CANDIDATE_INVALID_MARKER) {
        const char *invalid = primary_status == ROOT_CANDIDATE_INVALID_MARKER ? primary
                                                                              : alternative;

        fprintf(stderr,
                "Error: cup root marker is invalid for the recognized root '%s'. "
                "The other root candidate was preserved and was not selected.\n",
                invalid);
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (root_candidate_is_recognized(primary_status) &&
        root_candidate_is_recognized(alternative_status)) {
        fprintf(stderr,
                "Error: both cup root candidates are recognized: '%s' and '%s'.\n",
                primary,
                alternative);
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (root_candidate_is_recognized(primary_status)) {
        return text_copy(buffer, size, primary);
    }
    if (root_candidate_is_recognized(alternative_status)) {
        return text_copy(buffer, size, alternative);
    }
    if (primary_status == ROOT_CANDIDATE_MISSING) {
        return text_copy(buffer, size, primary);
    }
    if (alternative_status == ROOT_CANDIDATE_MISSING) {
        return text_copy(buffer, size, alternative);
    }

    fprintf(stderr,
            "Error: neither existing cup root candidate is recognized: '%s' or '%s'.\n",
            primary,
            alternative);
    return CUP_ERR_FILESYSTEM;
}

CupError layout_check_root_candidates(size_t *issue_count) {
    CupError err;
    char primary[MAX_PATH_LEN];
    char alternative[MAX_PATH_LEN];
    RootCandidateStatus primary_status;
    RootCandidateStatus alternative_status;

    if (issue_count == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *issue_count = 0;

    err = inspect_root_candidates(primary,
                                  sizeof(primary),
                                  &primary_status,
                                  alternative,
                                  sizeof(alternative),
                                  &alternative_status);
    if (err != CUP_OK) {
        return err;
    }

    if (primary_status == ROOT_CANDIDATE_DAMAGED) {
        printf("Issue: probable legacy cup root '%s' could not be verified.\n", primary);
        (*issue_count)++;
    }
    if (alternative_status == ROOT_CANDIDATE_DAMAGED) {
        printf("Issue: probable legacy cup root '%s' could not be verified.\n", alternative);
        (*issue_count)++;
    }
    if (primary_status == ROOT_CANDIDATE_INVALID_MARKER) {
        printf("Issue: cup root marker is invalid for recognized root '%s'.\n", primary);
        (*issue_count)++;
    }
    if (alternative_status == ROOT_CANDIDATE_INVALID_MARKER) {
        printf("Issue: cup root marker is invalid for recognized root '%s'.\n", alternative);
        (*issue_count)++;
    }
    if (*issue_count != 0) {
        printf("Info: neither cup root candidate was selected or modified.\n");
        return CUP_OK;
    }

    if (root_candidate_is_recognized(primary_status) &&
        root_candidate_is_recognized(alternative_status)) {
        printf("Issue: both cup root candidates are recognized: '%s' and '%s'.\n",
               primary,
               alternative);
        (*issue_count)++;
    } else if (!root_candidate_is_recognized(primary_status) &&
               !root_candidate_is_recognized(alternative_status) &&
               primary_status != ROOT_CANDIDATE_MISSING &&
               alternative_status != ROOT_CANDIDATE_MISSING) {
        printf("Issue: neither existing cup root candidate is recognized: '%s' or '%s'.\n",
               primary,
               alternative);
        (*issue_count)++;
    }
    return CUP_OK;
}

CupError layout_get_root_marker_path(char *buffer, size_t size) {
    return build_root_path(buffer, size, CUP_ROOT_MARKER_FILENAME);
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

/* Identity-bound paths for packages, cache entries, staging work and recovery evidence. */
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
        int marker_valid = 0;

        err = system_get_path_kind(root, &root_kind);
        if (err != CUP_OK) {
            return err;
        }
        if (root_kind == SYSTEM_PATH_DIRECTORY) {
            err = layout_get_root_marker_path(path, sizeof(path));
            if (err == CUP_OK) {
                err = root_marker_is_valid(root, &marker_valid);
            }
            if (err != CUP_OK) {
                return err;
            }
            if (!marker_valid) {
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

static CupError ensure_root_marker(const char *root) {
    CupError err;
    char marker[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN] = "";
    SystemPathKind kind;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    FILE *file = NULL;
    int valid;

    err = root_marker_is_valid(root, &valid);
    if (err != CUP_OK || valid) {
        return err;
    }
    err = build_candidate_path(marker, sizeof(marker), root, CUP_ROOT_MARKER_FILENAME);
    if (err == CUP_OK) {
        err = system_get_path_kind(marker, &kind);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (kind != SYSTEM_PATH_MISSING) {
        fprintf(stderr, "Error: cup root marker is invalid: '%s'.\n", marker);
        return CUP_ERR_VALIDATION;
    }

    err = system_create_temp_file(
        root, "root-marker", temporary, sizeof(temporary), &file);
    if (err != CUP_OK) {
        return err;
    }
    {
        int write_failed = fprintf(file,
                                   "format=%d\nproduct=%s\nlayout=%d\n",
                                   CUP_ROOT_MARKER_FORMAT,
                                   CUP_ROOT_MARKER_PRODUCT,
                                   CUP_ROOT_LAYOUT_FORMAT) < 0;
        int sync_failed = !write_failed && system_sync_file(file) != CUP_OK;
        int close_failed = fclose(file) != 0;

        file = NULL;
        if (write_failed || sync_failed || close_failed) {
            (void)system_remove_file(temporary);
            return CUP_ERR_FILESYSTEM;
        }
    }

    err = system_replace_file(temporary, marker, &commit_state);
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
            (void)system_remove_file(temporary);
        }
        return err;
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

    err = system_make_private_directory(root);
    return err == CUP_OK ? ensure_root_marker(root) : err;
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
