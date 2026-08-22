/*
 * Discovers a newer official release, verifies one immutable complete cup generation, stages
 * replacement assets and delegates the post-exit commit to the platform helper.
 */

#include "self_update.h"

#include "download.h"

#include "assets.h"
#include "checksum.h"
#include "command_context.h"
#include "filesystem.h"
#include "layout.h"
#include "install_policy.h"
#include "interrupt.h"
#include "package_catalog.h"
#include "path.h"
#include "system.h"
#include "text.h"
#include "update_journal.h"
#include "update_helper.h"
#include "runtime_journal.h"
#include "release_metadata.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CUP_VERSION_OFFICIAL

typedef struct {
    char binary_name[MAX_IDENTIFIER_LEN];
    char platform_checksums_name[MAX_IDENTIFIER_LEN];
    char staging[MAX_PATH_LEN];
    char staged_binary[MAX_PATH_LEN];
    char staged_platform_checksums[MAX_PATH_LEN];
    char staged_catalog[MAX_PATH_LEN];
    char staged_install_policy[MAX_PATH_LEN];
    char staged_common_checksums[MAX_PATH_LEN];
    char staged_metadata[MAX_PATH_LEN];
} UpdateFiles;

typedef struct {
    char binary[MAX_CATALOG_URL_LEN];
    char platform_checksums[MAX_CATALOG_URL_LEN];
    char catalog[MAX_CATALOG_URL_LEN];
    char install_policy[MAX_CATALOG_URL_LEN];
    char common_checksums[MAX_CATALOG_URL_LEN];
    char metadata[MAX_CATALOG_URL_LEN];
} UpdateUrls;

static int compare_versions(const ReleaseVersion *left, const ReleaseVersion *right) {
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

static CupError verify_downloaded_asset(const char *checksums,
                                        const char *asset_name,
                                        const char *path) {
    CupError err;
    int matches;

    err = checksum_verify_file(checksums, asset_name, path, &matches);
    if (err != CUP_OK) {
        return err;
    }
    if (!matches) {
        fprintf(stderr, "Error: downloaded asset '%s' failed checksum verification.\n", asset_name);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError build_latest_asset_url(char *url, size_t size, const char *asset) {
    char base[MAX_CATALOG_URL_LEN];
    CupError err;

    err = download_copy_release_base_override(base, sizeof(base));
    if (err == CUP_OK) {
        return text_format(url, size, "%s/%s", base, asset);
    }
    if (err != CUP_ERR_NOT_AVAILABLE) {
        return err;
    }
    return text_format(url, size, "%s/%s", CUP_RELEASE_LATEST_URL, asset);
}

static CupError build_release_asset_url(char *url,
                                        size_t size,
                                        const char *version,
                                        const char *asset) {
    char base[MAX_CATALOG_URL_LEN];
    CupError err;

    err = download_copy_release_base_override(base, sizeof(base));
    if (err == CUP_OK) {
        return text_format(url, size, "%s/%s/%s", base, version, asset);
    }
    if (err != CUP_ERR_NOT_AVAILABLE) {
        return err;
    }
    return text_format(url, size, CUP_RELEASE_VERSIONED_URL_TEMPLATE "/%s", version, asset);
}

/* The detached helper only replaces a complete current-generation install. */
static CupError require_replaceable_generation(void) {
    AssetsInspection inspection;
    CupError err = assets_inspect(&inspection);

    if (err != CUP_OK) {
        return err;
    }
    if (assets_installed_is_valid(&inspection)) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: the installed cup generation is missing or invalid. "
            "Run 'cup doctor' and 'cup repair' before 'cup update cup'.\n");
    return CUP_ERR_VALIDATION;
}

static CupError resolve_staged_update_paths(UpdateFiles *files,
                                            const char *staging_root) {
    CupError err;

    err = system_create_temp_directory(
        staging_root, "cup-update", files->staging, sizeof(files->staging));
    if (err == CUP_OK) {
        err = path_join(files->staged_binary,
                        sizeof(files->staged_binary),
                        files->staging,
                        CUP_UPDATE_BINARY_NEW);
    }
    if (err == CUP_OK) {
        err = path_join(files->staged_platform_checksums,
                        sizeof(files->staged_platform_checksums),
                        files->staging,
                        CUP_UPDATE_PLATFORM_CHECKSUMS_NEW);
    }
    if (err == CUP_OK) {
        err = path_join(files->staged_catalog,
                        sizeof(files->staged_catalog),
                        files->staging,
                        CUP_UPDATE_PACKAGES_NEW);
    }
    if (err == CUP_OK) {
        err = path_join(files->staged_install_policy,
                        sizeof(files->staged_install_policy),
                        files->staging,
                        CUP_UPDATE_INSTALL_POLICY_NEW);
    }
    if (err == CUP_OK) {
        err = path_join(files->staged_common_checksums,
                        sizeof(files->staged_common_checksums),
                        files->staging,
                        CUP_UPDATE_COMMON_CHECKSUMS_NEW);
    }
    if (err == CUP_OK) {
        err = path_join(files->staged_metadata,
                        sizeof(files->staged_metadata),
                        files->staging,
                        CUP_RELEASE_METADATA_FILENAME);
    }
    return err;
}

static CupError prepare_update_files(UpdateFiles *files) {
    char staging_root[MAX_PATH_LEN];
    CupError err;

    if (files == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(files, 0, sizeof(*files));

    err = assets_binary_asset_name(files->binary_name, sizeof(files->binary_name));
    if (err == CUP_OK) {
        err = assets_platform_checksums_name(files->platform_checksums_name,
                                            sizeof(files->platform_checksums_name));
    }
    if (err == CUP_OK) {
        err = layout_get_staging_dir(staging_root, sizeof(staging_root));
    }
    if (err != CUP_OK) {
        return err;
    }
    return resolve_staged_update_paths(files, staging_root);
}

/* Discovery step. The moving latest alias is used only to learn one concrete version and commit. */
static CupError discover_latest_release(const UpdateFiles *files,
                                        ReleaseMetadata *latest,
                                        int *update_available) {
    ReleaseVersion current_version;
    ReleaseVersion remote_version;
    char url[MAX_CATALOG_URL_LEN];
    CupError err;
    int comparison;

    if (files == NULL || latest == NULL || update_available == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *update_available = 0;

    err = build_latest_asset_url(url, sizeof(url), CUP_RELEASE_METADATA_FILENAME);
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
    err = release_metadata_load(files->staged_metadata, latest);
    if (err == CUP_OK) {
        err = release_version_parse(CUP_VERSION_BASE, &current_version);
    }
    if (err == CUP_OK) {
        err = release_version_parse(latest->version, &remote_version);
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

static CupError build_versioned_urls(UpdateUrls *urls,
                                     const UpdateFiles *files,
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

static CupError fetch_verified_release_metadata(const UpdateFiles *files,
                                                const UpdateUrls *urls,
                                                const ReleaseMetadata *latest,
                                                ReleaseMetadata *versioned) {
    const char *platform_assets[CUP_PLATFORM_CHECKSUM_ASSET_COUNT];
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
    platform_assets[1] = CUP_RELEASE_METADATA_FILENAME;
    platform_assets[2] = CUP_COMMON_CHECKSUMS_FILENAME;

    err = checksum_validate_assets(files->staged_platform_checksums,
                                   platform_assets,
                                   sizeof(platform_assets) / sizeof(platform_assets[0]));
    if (err == CUP_OK) {
        err = checksum_validate_assets(files->staged_common_checksums,
                                       CUP_COMMON_CHECKSUM_ASSETS,
                                       CUP_COMMON_CHECKSUM_ASSET_COUNT);
    }
    if (err == CUP_OK) {
        err = verify_downloaded_asset(files->staged_platform_checksums,
                                      CUP_COMMON_CHECKSUMS_FILENAME,
                                      files->staged_common_checksums);
    }
    if (err == CUP_OK) {
        err = verify_downloaded_asset(files->staged_platform_checksums,
                                      CUP_RELEASE_METADATA_FILENAME,
                                      files->staged_metadata);
    }
    if (err == CUP_OK) {
        err = release_metadata_load(files->staged_metadata, versioned);
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
static CupError fetch_verified_generation(const UpdateFiles *files, const UpdateUrls *urls) {
    PackageCatalog catalog;
    InstallPolicy install_policy;
    CupError err;

    err = download_file(urls->binary, files->staged_binary, DOWNLOAD_VALIDATE_BINARY);
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
    err = package_catalog_load_path(&catalog, files->staged_catalog);
    if (err == CUP_OK) {
        err = install_policy_load_path(&install_policy, files->staged_install_policy);
    }
    package_catalog_free(&catalog);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: downloaded cup configuration assets are invalid.\n");
    }
    return err;
}

static CupError prepare_staged_executables(const UpdateFiles *files) {
    if (files == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
#if defined(_WIN32)
    /* Windows executable semantics come from the canonical .exe destination. Internal staging
     * names intentionally use .new and must not be misclassified as command files. */
    return CUP_OK;
#else
    {
        return system_set_executable(files->staged_binary, 1);
    }
#endif
}

/* Public command used by `cup update cup` and by global update. */
CupError self_update_start(void) {
    CommandContext context = {0};
    UpdateFiles files;
    UpdateUrls urls;
    ReleaseMetadata latest_metadata;
    ReleaseMetadata versioned_metadata;
    UpdateJournal journal;
    CupError err;
    int update_available = 0;
    int transaction_started = 0;
    int helper_started = 0;
    char root[MAX_PATH_LEN];
    char helper_token[MAX_PATH_LEN];

    memset(&files, 0, sizeof(files));
    update_journal_init(&journal);

    /* Serialize discovery and handoff so no package operation observes half an update. */
    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }
    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        goto done;
    }
    err = require_replaceable_generation();
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
        err = update_helper_prepare();
    }
    if (err == CUP_OK) {
        err = update_journal_begin(
            files.staging, helper_token, versioned_metadata.version, &journal);
    }
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            transaction_started = 1;
        }
        goto done;
    }
    transaction_started = 1;

    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = update_helper_start(root, helper_token, &context.lock);
    }
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
            cleanup_err = journal.file_identity.valid
                              ? runtime_journal_clear_if_identity(&journal.file_identity)
                              : CUP_ERR_TRANSACTION;
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

CupError self_update_start(void) {
    fprintf(stderr,
            "Error: 'cup update cup' is available only from an official cup "
            "release; this build is '%s'.\n",
            CUP_VERSION);
    return CUP_ERR_INVALID_INPUT;
}

#endif
