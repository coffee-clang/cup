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
    const char *cursor;
    char *end;
    unsigned long parts[3];
    size_t i;

    if (text_is_empty(text) || version == NULL) {
        return CUP_ERR_VALIDATION;
    }

    cursor = text;
    for (i = 0; i < 3; ++i) {
        if (*cursor < '0' || *cursor > '9' ||
            (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')) {
            return CUP_ERR_VALIDATION;
        }
        parts[i] = strtoul(cursor, &end, 10);
        if (end == cursor || parts[i] > 999999u) {
            return CUP_ERR_VALIDATION;
        }
        if (i < 2) {
            if (*end != '.') {
                return CUP_ERR_VALIDATION;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return CUP_ERR_VALIDATION;
        }
    }

    version->major = (unsigned)parts[0];
    version->minor = (unsigned)parts[1];
    version->patch = (unsigned)parts[2];
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

static int commit_is_valid(const char *value) {
    size_t i;
    size_t length;

    if (text_is_empty(value)) {
        return 0;
    }
    length = strlen(value);
    if (length < 7 || length > 40) {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
            (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static CupError load_release_metadata(const char *path,
    ReleaseMetadata *metadata) {
    FILE *file;
    CupError err = CUP_OK;
    char line[256];
    size_t line_number = 0;
    unsigned seen = 0;

    if (text_is_empty(path) || metadata == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(metadata, 0, sizeof(*metadata));

    file = fopen(path, "r");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    while (err == CUP_OK) {
        char key[64];
        char value[128];
        unsigned bit;
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK || !has_line) {
            break;
        }
        if (text_parse_key_value(line, key, sizeof(key),
                value, sizeof(value)) != CUP_OK) {
            err = CUP_ERR_VALIDATION;
            break;
        }

        if (strcmp(key, "format") == 0) {
            bit = 1u << 0;
            if (strcmp(value, "1") != 0) {
                err = CUP_ERR_VALIDATION;
                break;
            }
        } else if (strcmp(key, "version") == 0) {
            bit = 1u << 1;
            if (text_copy(metadata->version, sizeof(metadata->version),
                    value) != CUP_OK) {
                err = CUP_ERR_VALIDATION;
                break;
            }
        } else if (strcmp(key, "commit") == 0) {
            bit = 1u << 2;
            if (text_copy(metadata->commit, sizeof(metadata->commit),
                    value) != CUP_OK) {
                err = CUP_ERR_VALIDATION;
                break;
            }
        } else {
            err = CUP_ERR_VALIDATION;
            break;
        }

        if ((seen & bit) != 0) {
            err = CUP_ERR_VALIDATION;
            break;
        }
        seen |= bit;
    }

    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err != CUP_OK) {
        return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
    }
    if (seen != 0x7u ||
        parse_version(metadata->version, &(SemanticVersion){0}) != CUP_OK ||
        !commit_is_valid(metadata->commit)) {
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

static CupError build_staging_path(const char *staging, const char *name,
    char *path, size_t size) {
    return path_join(path, size, staging, name);
}

static CupError build_release_asset_url(char *url, size_t size,
    const char *version, const char *asset) {
    return text_format(url, size,
        CUP_RELEASE_VERSIONED_URL_TEMPLATE "/%s", version, asset);
}

CupError command_self_update(void) {
    CommandContext context = {0};
    ReleaseMetadata latest_metadata;
    ReleaseMetadata versioned_metadata;
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
    char latest_metadata_url[MAX_MANIFEST_URL_LEN];
    const char *platform_assets[3];
    int transaction_started = 0;
    int helper_started = 0;

#if !CUP_VERSION_OFFICIAL
    fprintf(stderr,
        "Error: self-update is available only from an official cup release; "
        "this build is '%s'.\n", CUP_VERSION);
    return CUP_ERR_INVALID_INPUT;
#endif

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
        text_format(latest_metadata_url, sizeof(latest_metadata_url), "%s/%s",
            CUP_RELEASE_LATEST_URL, CUP_RELEASE_METADATA_FILENAME) != CUP_OK) {
        err = CUP_ERR_TEMPORARY;
        goto done;
    }

    printf("==> Checking for a cup update...\n");
    err = fetch_file(latest_metadata_url, staged_metadata,
        FETCH_VALIDATE_METADATA);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: the latest published release does not expose valid "
            "'%s' metadata. The release may be incomplete or unavailable.\n",
            CUP_RELEASE_METADATA_FILENAME);
        goto done;
    }
    err = load_release_metadata(staged_metadata, &latest_metadata);
    if (err != CUP_OK ||
        parse_version(CUP_VERSION_BASE, &current_version) != CUP_OK ||
        parse_version(latest_metadata.version, &remote_version) != CUP_OK) {
        fprintf(stderr, "Error: latest cup release metadata is invalid.\n");
        err = CUP_ERR_VALIDATION;
        goto done;
    }

    if (compare_versions(&remote_version, &current_version) <= 0) {
        if (compare_versions(&remote_version, &current_version) == 0) {
            printf("cup is already up to date at %s.\n", CUP_VERSION_BASE);
        } else {
            printf("Installed cup version %s is newer than the latest "
                "published release %s; no downgrade was applied.\n",
                CUP_VERSION_BASE, latest_metadata.version);
        }
        err = CUP_OK;
        goto done;
    }

    if (build_release_asset_url(checksums_url, sizeof(checksums_url),
            latest_metadata.version, checksums_name) != CUP_OK ||
        build_release_asset_url(metadata_url, sizeof(metadata_url),
            latest_metadata.version, CUP_RELEASE_METADATA_FILENAME) != CUP_OK ||
        build_release_asset_url(binary_url, sizeof(binary_url),
            latest_metadata.version, binary_name) != CUP_OK ||
        build_release_asset_url(uninstall_url, sizeof(uninstall_url),
            latest_metadata.version, CUP_UNINSTALL_FILENAME) != CUP_OK) {
        err = CUP_ERR_BUFFER_TOO_SMALL;
        goto done;
    }

    err = fetch_file(checksums_url, staged_checksums, FETCH_VALIDATE_METADATA);
    if (err != CUP_OK) {
        goto done;
    }
    err = fetch_file(metadata_url, staged_metadata, FETCH_VALIDATE_METADATA);
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
        load_release_metadata(staged_metadata, &versioned_metadata) != CUP_OK ||
        strcmp(latest_metadata.version, versioned_metadata.version) != 0 ||
        strcmp(latest_metadata.commit, versioned_metadata.commit) != 0) {
        fprintf(stderr, "Error: versioned cup release metadata is invalid.\n");
        err = CUP_ERR_VALIDATION;
        goto done;
    }

    printf("==> Downloading cup %s (installed: %s)...\n",
        versioned_metadata.version, CUP_VERSION_BASE);
    err = fetch_file(binary_url, staged_binary, FETCH_VALIDATE_BINARY);
    if (err != CUP_OK) {
        goto done;
    }
    err = fetch_file(uninstall_url, staged_uninstall,
        FETCH_VALIDATE_METADATA);
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

    printf("Verified update from cup %s to %s scheduled. The canonical "
        "assets will be replaced transactionally after this process exits.\n",
        CUP_VERSION_BASE, versioned_metadata.version);
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
