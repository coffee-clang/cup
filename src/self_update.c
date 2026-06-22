#include "commands.h"

#include "bootstrap.h"
#include "checksum.h"
#include "command_context.h"
#include "fetch.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"
#include "transaction.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
} SemanticVersion;

typedef struct {
    char version[64];
    char commit[64];
} ReleaseMetadata;

static CupError parse_version(const char *text, SemanticVersion *version) {
    char tail;

    if (text_is_empty(text) || version == NULL ||
        sscanf(text, "%u.%u.%u%c", &version->major, &version->minor,
            &version->patch, &tail) != 3) {
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static int compare_versions(const SemanticVersion *left,
    const SemanticVersion *right) {
    if (left->major != right->major) {
        return left->major < right->major ? -1 : 1;
    }
    if (left->minor != right->minor) {
        return left->minor < right->minor ? -1 : 1;
    }
    if (left->patch != right->patch) {
        return left->patch < right->patch ? -1 : 1;
    }
    return 0;
}

static CupError load_release_metadata(const char *path,
    ReleaseMetadata *metadata) {
    FILE *file;
    CupError err;
    char line[256];
    size_t line_number = 0;
    unsigned seen = 0;

    if (text_is_empty(path) || metadata == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(metadata, 0, sizeof(*metadata));

    file = fopen(path, "r");
    if (file == NULL) {
        return CUP_ERR_VALIDATION;
    }

    while (1) {
        char key[64];
        char value[128];
        unsigned bit;
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
        if (!has_line) {
            break;
        }
        if (text_parse_key_value(line, key, sizeof(key),
                value, sizeof(value)) != CUP_OK) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }

        if (strcmp(key, "format") == 0) {
            bit = 1u << 0;
            if (strcmp(value, "1") != 0) {
                fclose(file);
                return CUP_ERR_VALIDATION;
            }
        } else if (strcmp(key, "version") == 0) {
            bit = 1u << 1;
            if (text_copy(metadata->version, sizeof(metadata->version),
                    value) != CUP_OK) {
                fclose(file);
                return CUP_ERR_VALIDATION;
            }
        } else if (strcmp(key, "commit") == 0) {
            bit = 1u << 2;
            if (text_copy(metadata->commit, sizeof(metadata->commit),
                    value) != CUP_OK) {
                fclose(file);
                return CUP_ERR_VALIDATION;
            }
        } else {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }

        if ((seen & bit) != 0) {
            fclose(file);
            return CUP_ERR_VALIDATION;
        }
        seen |= bit;
    }

    if (fclose(file) != 0 || seen != 0x7u ||
        parse_version(metadata->version, &(SemanticVersion){0}) != CUP_OK ||
        text_is_empty(metadata->commit)) {
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError verify_downloaded_asset(const char *checksums,
    const char *asset_name, const char *path) {
    CupError err;
    int matches;

    err = bootstrap_verify_asset(checksums, asset_name, path, &matches);
    if (err != CUP_OK) {
        return err;
    }
    if (!matches) {
        fprintf(stderr,
            "Error: downloaded asset '%s' failed checksum verification.\n",
            asset_name);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError installed_asset_matches(const char *checksums,
    const char *asset_name, const char *path, int *matches) {
    int regular;
    CupError err;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;
    err = system_is_regular_file(path, &regular);
    if (err != CUP_OK || !regular) {
        return err;
    }
    return bootstrap_verify_asset(checksums, asset_name, path, matches);
}

static CupError build_staging_path(const char *staging, const char *name,
    char *path, size_t size) {
    return path_join(path, size, staging, name);
}

CupError command_self_update(void) {
    CommandContext context = {0};
    ReleaseMetadata metadata;
    SemanticVersion current_version;
    SemanticVersion remote_version;
    CupError err;
    char binary_name[MAX_NAME_LEN];
    char checksums_name[MAX_NAME_LEN];
    char tmp_dir[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN] = "";
    char staged_binary[MAX_PATH_LEN];
    char staged_uninstall[MAX_PATH_LEN];
    char staged_checksums[MAX_PATH_LEN];
    char staged_metadata[MAX_PATH_LEN];
    char installed_binary[MAX_PATH_LEN];
    char installed_uninstall[MAX_PATH_LEN];
    char installed_checksums[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];
    char binary_url[MAX_MANIFEST_URL_LEN];
    char uninstall_url[MAX_MANIFEST_URL_LEN];
    char checksums_url[MAX_MANIFEST_URL_LEN];
    char metadata_url[MAX_MANIFEST_URL_LEN];
    const char *platform_assets[3];
    int binary_matches;
    int uninstall_matches;
    int transaction_started = 0;
    int helper_started = 0;

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }
    err = command_require_no_transaction();
    if (err != CUP_OK) {
        goto done;
    }

    if (bootstrap_binary_asset_name(binary_name, sizeof(binary_name)) != CUP_OK ||
        bootstrap_platform_checksums_name(checksums_name,
            sizeof(checksums_name)) != CUP_OK ||
        layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        layout_get_binary_path(installed_binary,
            sizeof(installed_binary)) != CUP_OK ||
        layout_get_uninstall_path(installed_uninstall,
            sizeof(installed_uninstall)) != CUP_OK ||
        layout_get_platform_checksums_path(installed_checksums,
            sizeof(installed_checksums)) != CUP_OK ||
        layout_get_lock_path(lock_path, sizeof(lock_path)) != CUP_OK ||
        layout_get_transaction_path(journal_path, sizeof(journal_path)) != CUP_OK ||
        system_create_temp_directory(tmp_dir, "self-update", staging,
            sizeof(staging)) != CUP_OK ||
        build_staging_path(staging, CUP_SELF_UPDATE_BINARY_NEW,
            staged_binary, sizeof(staged_binary)) != CUP_OK ||
        build_staging_path(staging, CUP_SELF_UPDATE_UNINSTALL_NEW,
            staged_uninstall, sizeof(staged_uninstall)) != CUP_OK ||
        build_staging_path(staging, CUP_SELF_UPDATE_CHECKSUMS_NEW,
            staged_checksums, sizeof(staged_checksums)) != CUP_OK ||
        build_staging_path(staging, CUP_RELEASE_METADATA_FILENAME,
            staged_metadata, sizeof(staged_metadata)) != CUP_OK ||
        text_format(checksums_url, sizeof(checksums_url), "%s/%s",
            CUP_RELEASE_URL, checksums_name) != CUP_OK ||
        text_format(binary_url, sizeof(binary_url), "%s/%s",
            CUP_RELEASE_URL, binary_name) != CUP_OK ||
        text_format(uninstall_url, sizeof(uninstall_url), "%s/%s",
            CUP_RELEASE_URL, CUP_UNINSTALL_FILENAME) != CUP_OK ||
        text_format(metadata_url, sizeof(metadata_url), "%s/%s",
            CUP_RELEASE_URL, CUP_RELEASE_METADATA_FILENAME) != CUP_OK) {
        err = CUP_ERR_TEMPORARY;
        goto done;
    }

    printf("==> Checking for a cup update...\n");
    err = fetch_file(checksums_url, staged_checksums, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }
    err = fetch_file(metadata_url, staged_metadata, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }

    platform_assets[0] = binary_name;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    platform_assets[2] = CUP_RELEASE_METADATA_FILENAME;
    err = checksum_validate_assets(staged_checksums, platform_assets,
        sizeof(platform_assets) / sizeof(platform_assets[0]));
    if (err != CUP_OK ||
        verify_downloaded_asset(staged_checksums,
            CUP_RELEASE_METADATA_FILENAME, staged_metadata) != CUP_OK ||
        load_release_metadata(staged_metadata, &metadata) != CUP_OK ||
        parse_version(CUP_VERSION_BASE, &current_version) != CUP_OK ||
        parse_version(metadata.version, &remote_version) != CUP_OK) {
        fprintf(stderr, "Error: remote release metadata is invalid.\n");
        err = CUP_ERR_VALIDATION;
        goto done;
    }

    if (compare_versions(&remote_version, &current_version) < 0) {
        printf("Installed cup version %s is newer than the latest published "
            "release %s; no update was applied.\n",
            CUP_VERSION, metadata.version);
        err = CUP_OK;
        goto done;
    }

    err = installed_asset_matches(staged_checksums, binary_name,
        installed_binary, &binary_matches);
    if (err != CUP_OK) {
        goto done;
    }
    err = installed_asset_matches(staged_checksums, CUP_UNINSTALL_FILENAME,
        installed_uninstall, &uninstall_matches);
    if (err != CUP_OK) {
        goto done;
    }

    if (compare_versions(&remote_version, &current_version) == 0 &&
        binary_matches && uninstall_matches) {
        printf("The canonical cup installation is already up to date at %s.\n",
            metadata.version);
        err = CUP_OK;
        goto done;
    }

    printf("==> Downloading cup %s (installed build: %s)...\n",
        metadata.version, CUP_VERSION);
    err = fetch_file(binary_url, staged_binary, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }
    err = fetch_file(uninstall_url, staged_uninstall, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }
    if (verify_downloaded_asset(staged_checksums, binary_name,
            staged_binary) != CUP_OK ||
        verify_downloaded_asset(staged_checksums, CUP_UNINSTALL_FILENAME,
            staged_uninstall) != CUP_OK) {
        err = CUP_ERR_VALIDATION;
        goto done;
    }

#if !defined(_WIN32)
    if (system_set_executable(staged_binary, 1) != CUP_OK ||
        system_set_executable(staged_uninstall, 1) != CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
        goto done;
    }
#endif

    err = transaction_begin_self_update(staging);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            transaction_started = 1;
        }
        goto done;
    }
    transaction_started = 1;

    err = system_start_self_update(staging, installed_binary,
        installed_uninstall, installed_checksums, lock_path, journal_path,
        system_get_process_id());
    if (err != CUP_OK) {
        goto done;
    }
    helper_started = 1;

    printf("Verified cup %s update scheduled. The canonical assets will be "
        "replaced transactionally after this process exits.\n",
        metadata.version);
    err = CUP_OK;

done:
    if (!helper_started && staging[0] != '\0') {
        CupError cleanup_err = CUP_OK;

        if (transaction_started) {
            cleanup_err = transaction_clear();
        }
        if (cleanup_err == CUP_OK) {
            cleanup_err = filesystem_remove_tree(staging);
        }
        if (cleanup_err != CUP_OK) {
            fprintf(stderr,
                "Error: self-update cleanup was incomplete. Run 'cup repair'.\n");
            err = CUP_ERR_TRANSACTION;
        }
    }
    command_context_end(&context);
    return err;
}
