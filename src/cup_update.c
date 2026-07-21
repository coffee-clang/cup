/*
 * Discovers a newer official release, verifies one immutable complete cup generation, stages
 * replacement assets and delegates the post-exit commit to the platform helper.
 */

#include "commands.h"
#include "download.h"

#include "cup_assets.h"
#include "checksum.h"
#include "command_context.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "package_catalog.h"
#include "path.h"
#include "system.h"
#include "text.h"
#include "cup_update_journal.h"
#include "cup_update_helper.h"
#include "runtime_journal.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CUP_VERSION_OFFICIAL

/* Parsed official version used only for monotonic update comparison. */
typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
} SemanticVersion;

typedef struct {
    char version[64];
    char commit[64];
} ReleaseMetadata;

typedef struct {
    char binary_name[MAX_IDENTIFIER_LEN];
    char platform_checksums_name[MAX_IDENTIFIER_LEN];
    char staging[MAX_PATH_LEN];
    char staged_binary[MAX_PATH_LEN];
    char staged_uninstall[MAX_PATH_LEN];
    char staged_platform_checksums[MAX_PATH_LEN];
    char staged_catalog[MAX_PATH_LEN];
    char staged_install_policy[MAX_PATH_LEN];
    char staged_common_checksums[MAX_PATH_LEN];
    char staged_metadata[MAX_PATH_LEN];
    char installed_binary[MAX_PATH_LEN];
    char installed_uninstall[MAX_PATH_LEN];
    char installed_platform_checksums[MAX_PATH_LEN];
    char installed_catalog[MAX_PATH_LEN];
    char installed_install_policy[MAX_PATH_LEN];
    char installed_common_checksums[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char journal_path[MAX_PATH_LEN];
} CupUpdateFiles;

typedef struct {
    char binary[MAX_CATALOG_URL_LEN];
    char uninstall[MAX_CATALOG_URL_LEN];
    char platform_checksums[MAX_CATALOG_URL_LEN];
    char catalog[MAX_CATALOG_URL_LEN];
    char install_policy[MAX_CATALOG_URL_LEN];
    char common_checksums[MAX_CATALOG_URL_LEN];
    char metadata[MAX_CATALOG_URL_LEN];
} CupUpdateUrls;

/* Release metadata parsing. Public versions and commits are validated before monotonic comparison
 * or URL construction. */
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

static int compare_versions(const SemanticVersion *left, const SemanticVersion *right) {
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
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static CupError load_release_metadata(const char *path, ReleaseMetadata *metadata) {
    SemanticVersion parsed_version;
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
        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK) {
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
            if (text_copy(metadata->version, sizeof(metadata->version), value) != CUP_OK) {
                err = CUP_ERR_VALIDATION;
                break;
            }
        } else if (strcmp(key, "commit") == 0) {
            bit = 1u << 2;
            if (text_copy(metadata->commit, sizeof(metadata->commit), value) != CUP_OK) {
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
    if (seen != 0x7u || parse_version(metadata->version, &parsed_version) != CUP_OK ||
        !commit_is_valid(metadata->commit)) {
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError verify_downloaded_asset(const char *checksums,
                                        const char *asset_name,
                                        const char *path) {
    CupError err;
    int matches;

    err = cup_assets_verify_asset(checksums, asset_name, path, &matches);
    if (err != CUP_OK) {
        return err;
    }
    if (!matches) {
        fprintf(stderr, "Error: downloaded asset '%s' failed checksum verification.\n", asset_name);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError build_staging_path(const char *staging, const char *name, char *path, size_t size) {
    return path_join(path, size, staging, name);
}

static CupError build_release_asset_url(char *url,
                                        size_t size,
                                        const char *version,
                                        const char *asset) {
    return text_format(url, size, CUP_RELEASE_VERSIONED_URL_TEMPLATE "/%s", version, asset);
}

/* The detached helper only replaces a complete current-generation install. */
static CupError require_replaceable_installed_generation(void) {
    CupAssetsInspection inspection;
    CupError err = cup_assets_inspect(&inspection);

    if (err != CUP_OK) {
        return err;
    }
    if (cup_assets_installed_is_valid(&inspection)) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: the installed cup generation is missing or invalid. "
            "Run 'cup doctor' and 'cup repair' before 'cup update cup'.\n");
    return CUP_ERR_VALIDATION;
}

static CupError prepare_update_files(CupUpdateFiles *files) {
    char tmp_dir[MAX_PATH_LEN];
    CupError err;

    if (files == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(files, 0, sizeof(*files));

    /* Resolve canonical installed paths and the names used by the published release. */
    err = cup_assets_binary_asset_name(files->binary_name, sizeof(files->binary_name));
    if (err == CUP_OK) {
        err = cup_assets_platform_checksums_name(files->platform_checksums_name,
                                                 sizeof(files->platform_checksums_name));
    }
    if (err == CUP_OK) {
        err = layout_get_staging_dir(tmp_dir, sizeof(tmp_dir));
    }
    if (err == CUP_OK) {
        err = layout_get_binary_path(files->installed_binary, sizeof(files->installed_binary));
    }
    if (err == CUP_OK) {
        err = layout_get_uninstall_path(files->installed_uninstall,
                                        sizeof(files->installed_uninstall));
    }
    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(files->installed_platform_checksums,
                                                 sizeof(files->installed_platform_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(files->installed_catalog,
                                              sizeof(files->installed_catalog));
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(files->installed_install_policy,
                                             sizeof(files->installed_install_policy));
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(files->installed_common_checksums,
                                               sizeof(files->installed_common_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_lock_path(files->lock_path, sizeof(files->lock_path));
    }
    if (err == CUP_OK) {
        err = layout_get_transaction_path(files->journal_path, sizeof(files->journal_path));
    }
    /* Allocate one private staging generation, then derive every staged asset path from it. */
    if (err == CUP_OK) {
        err = system_create_temp_directory(
            tmp_dir, "cup-update", files->staging, sizeof(files->staging));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_BINARY_NEW,
                                 files->staged_binary,
                                 sizeof(files->staged_binary));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_UNINSTALL_NEW,
                                 files->staged_uninstall,
                                 sizeof(files->staged_uninstall));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_PLATFORM_CHECKSUMS_NEW,
                                 files->staged_platform_checksums,
                                 sizeof(files->staged_platform_checksums));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_PACKAGES_NEW,
                                 files->staged_catalog,
                                 sizeof(files->staged_catalog));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_INSTALL_POLICY_NEW,
                                 files->staged_install_policy,
                                 sizeof(files->staged_install_policy));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_UPDATE_COMMON_CHECKSUMS_NEW,
                                 files->staged_common_checksums,
                                 sizeof(files->staged_common_checksums));
    }
    if (err == CUP_OK) {
        err = build_staging_path(files->staging,
                                 CUP_RELEASE_METADATA_FILENAME,
                                 files->staged_metadata,
                                 sizeof(files->staged_metadata));
    }
    return err;
}

/* Discovery step. The moving latest alias is used only to learn one concrete version and commit. */
static CupError discover_latest_release(const CupUpdateFiles *files,
                                        ReleaseMetadata *latest,
                                        int *update_available) {
    SemanticVersion current_version;
    SemanticVersion remote_version;
    char url[MAX_CATALOG_URL_LEN];
    CupError err;
    int comparison;

    if (files == NULL || latest == NULL || update_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *update_available = 0;

    err = text_format(
        url, sizeof(url), "%s/%s", CUP_RELEASE_LATEST_URL, CUP_RELEASE_METADATA_FILENAME);
    if (err != CUP_OK) {
        return err;
    }

    printf("==> Checking for a cup update...\n");
    err = download_file(url, files->staged_metadata, DOWNLOAD_VALIDATE_METADATA);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: the latest published release does not expose valid "
                "'%s' metadata. The release may be incomplete or unavailable.\n",
                CUP_RELEASE_METADATA_FILENAME);
        return err;
    }
    err = load_release_metadata(files->staged_metadata, latest);
    if (err == CUP_OK) {
        err = parse_version(CUP_VERSION_BASE, &current_version);
    }
    if (err == CUP_OK) {
        err = parse_version(latest->version, &remote_version);
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: latest cup release metadata is invalid.\n");
        return CUP_ERR_VALIDATION;
    }

    comparison = compare_versions(&remote_version, &current_version);
    if (comparison == 0) {
        printf("cup is already up to date at %s.\n", CUP_VERSION_BASE);
        return CUP_OK;
    }
    if (comparison < 0) {
        printf("Installed cup version %s is newer than the latest "
               "published release %s; no downgrade was applied.\n",
               CUP_VERSION_BASE,
               latest->version);
        return CUP_OK;
    }

    *update_available = 1;
    return CUP_OK;
}

static CupError build_versioned_urls(CupUpdateUrls *urls,
                                     const CupUpdateFiles *files,
                                     const char *version) {
    CupError err;

    if (urls == NULL || files == NULL || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(urls, 0, sizeof(*urls));

    err = build_release_asset_url(urls->platform_checksums,
                                  sizeof(urls->platform_checksums),
                                  version,
                                  files->platform_checksums_name);
    if (err == CUP_OK) {
        err = build_release_asset_url(urls->common_checksums,
                                      sizeof(urls->common_checksums),
                                      version,
                                      CUP_COMMON_CHECKSUMS_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_release_asset_url(
            urls->metadata, sizeof(urls->metadata), version, CUP_RELEASE_METADATA_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_release_asset_url(
            urls->binary, sizeof(urls->binary), version, files->binary_name);
    }
    if (err == CUP_OK) {
        err = build_release_asset_url(
            urls->uninstall, sizeof(urls->uninstall), version, CUP_UNINSTALL_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_release_asset_url(
            urls->catalog, sizeof(urls->catalog), version, CUP_PACKAGES_FILENAME);
    }
    if (err == CUP_OK) {
        err = build_release_asset_url(urls->install_policy,
                                      sizeof(urls->install_policy),
                                      version,
                                      CUP_INSTALL_POLICY_FILENAME);
    }
    return err;
}

static CupError fetch_verified_release_metadata(const CupUpdateFiles *files,
                                                const CupUpdateUrls *urls,
                                                const ReleaseMetadata *latest,
                                                ReleaseMetadata *versioned) {
    const char *platform_assets[3];
    const char *common_assets[2];
    CupError err;

    err = download_file(
        urls->platform_checksums, files->staged_platform_checksums, DOWNLOAD_VALIDATE_METADATA);
    if (err == CUP_OK) {
        err = download_file(
            urls->common_checksums, files->staged_common_checksums, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err == CUP_OK) {
        err = download_file(urls->metadata, files->staged_metadata, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err != CUP_OK) {
        return err;
    }

    platform_assets[0] = files->binary_name;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    platform_assets[2] = CUP_RELEASE_METADATA_FILENAME;
    common_assets[0] = CUP_PACKAGES_FILENAME;
    common_assets[1] = CUP_INSTALL_POLICY_FILENAME;

    err = checksum_validate_assets(files->staged_platform_checksums,
                                   platform_assets,
                                   sizeof(platform_assets) / sizeof(platform_assets[0]));
    if (err == CUP_OK) {
        err = checksum_validate_assets(files->staged_common_checksums,
                                       common_assets,
                                       sizeof(common_assets) / sizeof(common_assets[0]));
    }
    if (err == CUP_OK) {
        err = verify_downloaded_asset(files->staged_platform_checksums,
                                      CUP_RELEASE_METADATA_FILENAME,
                                      files->staged_metadata);
    }
    if (err == CUP_OK) {
        err = load_release_metadata(files->staged_metadata, versioned);
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: versioned cup release metadata is invalid.\n");
        return err;
    }
    if (strcmp(latest->version, versioned->version) != 0 ||
        strcmp(latest->commit, versioned->commit) != 0) {
        fprintf(stderr, "Error: versioned cup release metadata is invalid.\n");
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

/* Generation download. Every asset is fetched from the immutable version tag and checked before
 * helper handoff. */
static CupError fetch_verified_generation(const CupUpdateFiles *files, const CupUpdateUrls *urls) {
    PackageCatalog catalog;
    InstallPolicy install_policy;
    CupError err;

    err = download_file(urls->binary, files->staged_binary, DOWNLOAD_VALIDATE_BINARY);
    if (err == CUP_OK) {
        err = download_file(urls->uninstall, files->staged_uninstall, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err == CUP_OK) {
        err = download_file(urls->catalog, files->staged_catalog, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err == CUP_OK) {
        err = download_file(
            urls->install_policy, files->staged_install_policy, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = verify_downloaded_asset(
        files->staged_platform_checksums, files->binary_name, files->staged_binary);
    if (err == CUP_OK) {
        err = verify_downloaded_asset(
            files->staged_platform_checksums, CUP_UNINSTALL_FILENAME, files->staged_uninstall);
    }
    if (err == CUP_OK) {
        err = verify_downloaded_asset(
            files->staged_common_checksums, CUP_PACKAGES_FILENAME, files->staged_catalog);
    }
    if (err == CUP_OK) {
        err = verify_downloaded_asset(files->staged_common_checksums,
                                      CUP_INSTALL_POLICY_FILENAME,
                                      files->staged_install_policy);
    }
    if (err != CUP_OK) {
        return err;
    }

    package_catalog_init(&catalog);
    install_policy_init(&install_policy);
    err = package_catalog_load_path(
        &catalog, files->staged_catalog, PACKAGE_CATALOG_SOURCE_INSTALLED);
    if (err == CUP_OK) {
        err = install_policy_load_path(
            &install_policy, files->staged_install_policy, INSTALL_POLICY_SOURCE_INSTALLED);
    }
    package_catalog_free(&catalog);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: downloaded cup configuration assets are invalid.\n");
    }
    return err;
}

static CupError prepare_staged_executables(const CupUpdateFiles *files) {
#if !defined(_WIN32)
    CupError err = system_set_executable(files->staged_binary, 1);

    if (err == CUP_OK) {
        err = system_set_executable(files->staged_uninstall, 1);
    }
    return err;
#else
    (void)files;
    return CUP_OK;
#endif
}

/* Public command used by `cup update cup` and by global update. */
CupError command_update_cup(void) {
    CommandContext context = {0};
    CupUpdateFiles files;
    CupUpdateUrls urls;
    ReleaseMetadata latest_metadata;
    ReleaseMetadata versioned_metadata;
    CupError err;
    int update_available = 0;
    int transaction_started = 0;
    int helper_started = 0;
    char helper_token[MAX_PATH_LEN];

    memset(&files, 0, sizeof(files));

    /* Serialize discovery and handoff so no package operation observes half an update. */
    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }
    err = runtime_journal_require_none();
    if (err == CUP_OK) {
        err = require_replaceable_installed_generation();
    }
    if (err == CUP_OK) {
        err = prepare_update_files(&files);
    }
    if (err == CUP_OK) {
        err = discover_latest_release(&files, &latest_metadata, &update_available);
    }
    if (err != CUP_OK || !update_available) {
        goto done;
    }

    /* Once latest resolves to a concrete release, fetch only immutable versioned assets. */
    err = build_versioned_urls(&urls, &files, latest_metadata.version);
    if (err == CUP_OK) {
        err = fetch_verified_release_metadata(&files, &urls, &latest_metadata, &versioned_metadata);
    }
    if (err == CUP_OK) {
        printf("==> Downloading cup %s (installed: %s)...\n",
               versioned_metadata.version,
               CUP_VERSION_BASE);
        err = fetch_verified_generation(&files, &urls);
    }
    if (err == CUP_OK) {
        err = prepare_staged_executables(&files);
    }
    if (err != CUP_OK) {
        goto done;
    }

    /* Persist the handoff before starting the detached helper that commits after parent exit. */
    err = text_format(helper_token,
                      sizeof(helper_token),
                      "u%lu-%s",
                      system_get_process_id(),
                      path_last_segment(files.staging));
    if (err == CUP_OK) {
        err = cup_update_helper_prepare();
    }
    if (err == CUP_OK) {
        err = cup_update_journal_begin(files.staging, helper_token, versioned_metadata.version);
    }
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            transaction_started = 1;
        }
        goto done;
    }
    transaction_started = 1;

    err = cup_update_helper_start(helper_token);
    if (err != CUP_OK) {
        goto done;
    }
    helper_started = 1;

    printf("Verified update from cup %s to %s scheduled. The executable and "
           "official configuration assets will be replaced transactionally "
           "after this process exits.\n",
           CUP_VERSION_BASE,
           versioned_metadata.version);
    err = CUP_OK;

done:
    /* Before helper ownership, this process remains responsible for journal and staging cleanup. */
    if (!helper_started && files.staging[0] != '\0') {
        CupError cleanup_err = CUP_OK;

        if (transaction_started) {
            cleanup_err = cup_update_journal_clear();
        }
        if (cleanup_err == CUP_OK) {
            cleanup_err = filesystem_remove_tree(files.staging);
        }
        if (cleanup_err != CUP_OK) {
            fprintf(stderr, "Error: cup update cleanup was incomplete. Run 'cup repair'.\n");
            err = CUP_ERR_TRANSACTION;
        }
    }
    command_context_end(&context);
    return err;
}

#else

CupError command_update_cup(void) {
    fprintf(stderr,
            "Error: 'cup update cup' is available only from an official cup "
            "release; this build is '%s'.\n",
            CUP_VERSION);
    return CUP_ERR_INVALID_INPUT;
}

#endif
