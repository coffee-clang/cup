/* Discovers, authenticates and stages one complete CUP generation before detached handoff. */

#include "self_update.h"

#include "checksum.h"
#include "command_context.h"
#include "constants.h"
#include "download.h"
#include "filesystem.h"
#include "generation.h"
#include "interrupt.h"
#include "layout.h"
#include "path.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"
#include "update_helper.h"
#include "update_journal.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#if CUP_VERSION_OFFICIAL

typedef struct {
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    char discovery[MAX_PATH_LEN];
    char release[MAX_PATH_LEN];
    char license[MAX_PATH_LEN];
    char notices[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char binary_name[MAX_PATH_SEGMENT_LEN];
} UpdateFiles;

static int compare_versions(const ReleaseVersion *left, const ReleaseVersion *right) {
    if (left->major != right->major) return left->major < right->major ? -1 : 1;
    if (left->minor != right->minor) return left->minor < right->minor ? -1 : 1;
    if (left->patch != right->patch) return left->patch < right->patch ? -1 : 1;
    return 0;
}

static CupError build_latest_asset_url(char *url, size_t size, const char *asset) {
    char base[MAX_CATALOG_URL_LEN];
    CupError err = download_copy_release_base_override(base, sizeof(base));
    if (err == CUP_OK) return text_format(url, size, "%s/%s", base, asset);
    if (err != CUP_ERR_NOT_AVAILABLE) return err;
    return text_format(url, size, "%s/%s", CUP_RELEASE_LATEST_URL, asset);
}

static CupError build_release_asset_url(char *url,
                                        size_t size,
                                        const char *version,
                                        const char *asset) {
    char base[MAX_CATALOG_URL_LEN];
    CupError err = download_copy_release_base_override(base, sizeof(base));
    if (err == CUP_OK) return text_format(url, size, "%s/%s/%s", base, version, asset);
    if (err != CUP_ERR_NOT_AVAILABLE) return err;
    return text_format(url, size, CUP_RELEASE_VERSIONED_URL_TEMPLATE "/%s", version, asset);
}

static CupError verify_file_digest(const char *path, const char *expected) {
    char actual[65];
    CupError err;
    if (text_is_empty(path) || !checksum_digest_is_canonical(expected)) return CUP_ERR_INVALID_INPUT;
    err = checksum_sha256_file(path, actual, sizeof(actual));
    return err == CUP_OK && strcmp(actual, expected) == 0 ? CUP_OK
           : err != CUP_OK ? err : CUP_ERR_VALIDATION;
}

static CupError require_trusted_current_generation(void) {
    GenerationAssetSpec release_spec;
    GenerationAssetSpec binary_spec;
    ReleaseMetadata metadata;
    const ReleaseAsset *binary_asset;
    CupError err;
    int binary_mismatch = 0;

    release_metadata_init(&metadata);
    err = generation_asset_spec(CUP_GENERATION_ASSET_RELEASE, &release_spec);
    if (err == CUP_OK) err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &binary_spec);
    if (err == CUP_OK) err = release_metadata_load(release_spec.destination, &metadata);
    if (err == CUP_OK) err = generation_validate_manifest(&metadata);
    if (err == CUP_OK && strcmp(metadata.version, CUP_VERSION_BASE) != 0) err = CUP_ERR_VALIDATION;
    binary_asset = err == CUP_OK
                       ? generation_manifest_asset(&metadata, CUP_GENERATION_ASSET_BINARY)
                       : NULL;
    if (err == CUP_OK && binary_asset == NULL) err = CUP_ERR_VALIDATION;
    if (err == CUP_OK) {
        err = verify_file_digest(binary_spec.destination, binary_asset->sha256);
        binary_mismatch = err != CUP_OK;
    }
    release_metadata_free(&metadata);
    if (err != CUP_OK) {
        if (binary_mismatch) {
            fprintf(stderr,
                    "Error: the installed CUP binary does not match its release manifest. "
                    "Use the official installer to repair the CUP generation before 'cup update cup'.\n");
        } else {
            fprintf(stderr,
                    "Error: the installed CUP generation metadata is not trustworthy. "
                    "Run 'cup repair'; if the binary itself is damaged, use the official installer.\n");
        }
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError prepare_update_files(UpdateFiles *files) {
    char staging_root[MAX_PATH_LEN];
    GenerationAssetSpec spec;
    CupError err;
    if (files == NULL) return CUP_ERR_INVALID_INPUT;
    memset(files, 0, sizeof(*files));
    err = layout_get_staging_dir(staging_root, sizeof(staging_root));
    if (err == CUP_OK) {
        err = system_create_temp_directory(staging_root, CUP_UPDATE_TEMP_PREFIX,
                                           files->staging, sizeof(files->staging));
    }
    if (err == CUP_OK) err = path_join(files->new_dir, sizeof(files->new_dir),
                                        files->staging, CUP_UPDATE_NEW_DIRECTORY);
    if (err == CUP_OK) err = filesystem_ensure_directory(files->new_dir);
    if (err == CUP_OK) err = path_join(files->discovery, sizeof(files->discovery),
                                        files->staging, "latest-release.txt");
    if (err == CUP_OK) err = generation_asset_spec(CUP_GENERATION_ASSET_RELEASE, &spec);
    if (err == CUP_OK) err = path_join(files->release, sizeof(files->release), files->new_dir,
                                        spec.release_name);
    if (err == CUP_OK) err = generation_asset_spec(CUP_GENERATION_ASSET_LICENSE, &spec);
    if (err == CUP_OK) err = path_join(files->license, sizeof(files->license), files->new_dir,
                                        spec.release_name);
    if (err == CUP_OK) err = generation_asset_spec(CUP_GENERATION_ASSET_NOTICES, &spec);
    if (err == CUP_OK) err = path_join(files->notices, sizeof(files->notices), files->new_dir,
                                        spec.release_name);
    if (err == CUP_OK) err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &spec);
    if (err == CUP_OK) err = text_copy(files->binary_name, sizeof(files->binary_name), spec.release_name);
    if (err == CUP_OK) err = path_join(files->binary, sizeof(files->binary), files->new_dir,
                                        spec.release_name);
    return err;
}

static CupError discover_target(const UpdateFiles *files,
                                ReleaseMetadata *latest,
                                int *update_available) {
    ReleaseVersion current_version;
    ReleaseVersion remote_version;
    char url[MAX_CATALOG_URL_LEN];
    CupError err;
    int comparison;
    if (files == NULL || latest == NULL || update_available == NULL) return CUP_ERR_INVALID_INPUT;
    *update_available = 0;
    err = build_latest_asset_url(url, sizeof(url), CUP_RELEASE_METADATA_FILENAME);
    if (err == CUP_OK) {
        printf("==> Checking for a CUP update...\n");
        err = download_file(url, files->discovery, DOWNLOAD_VALIDATE_METADATA);
    }
    if (err == CUP_OK) err = release_metadata_load(files->discovery, latest);
    if (err == CUP_OK) err = generation_validate_manifest(latest);
    if (err == CUP_OK) err = release_version_parse(CUP_VERSION_BASE, &current_version);
    if (err == CUP_OK) err = release_version_parse(latest->version, &remote_version);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: latest CUP release metadata is invalid or unavailable.\n");
        return err;
    }
    comparison = compare_versions(&remote_version, &current_version);
    if (comparison == 0) {
        printf("CUP is already up to date at %s.\n", CUP_VERSION_BASE);
        return CUP_OK;
    }
    if (comparison < 0) {
        printf("Installed CUP version %s is newer than published release %s; no downgrade was applied.\n",
               CUP_VERSION_BASE, latest->version);
        return CUP_OK;
    }
    *update_available = 1;
    return CUP_OK;
}

static CupError fetch_versioned_target(const UpdateFiles *files,
                                       const ReleaseMetadata *latest,
                                       ReleaseMetadata *target) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    const char *paths[CUP_GENERATION_ASSET_COUNT];
    char url[MAX_CATALOG_URL_LEN];
    CupError err;
    size_t i;

    paths[CUP_GENERATION_ASSET_RELEASE] = files->release;
    paths[CUP_GENERATION_ASSET_LICENSE] = files->license;
    paths[CUP_GENERATION_ASSET_NOTICES] = files->notices;
    paths[CUP_GENERATION_ASSET_BINARY] = files->binary;
    err = generation_asset_specs(specs);
    if (err == CUP_OK) {
        err = build_release_asset_url(url, sizeof(url), latest->version,
                                      specs[CUP_GENERATION_ASSET_RELEASE].release_name);
    }
    if (err == CUP_OK) err = download_file(url, files->release, DOWNLOAD_VALIDATE_METADATA);
    if (err == CUP_OK) err = release_metadata_load(files->release, target);
    if (err == CUP_OK) err = generation_validate_manifest(target);
    if (err == CUP_OK && (strcmp(target->version, latest->version) != 0 ||
                          strcmp(target->commit, latest->commit) != 0)) {
        err = CUP_ERR_VALIDATION;
    }
    for (i = CUP_GENERATION_ASSET_LICENSE; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        const ReleaseAsset *asset = generation_manifest_asset(target, (GenerationAssetId)i);
        if (asset == NULL) { err = CUP_ERR_VALIDATION; break; }
        err = build_release_asset_url(url, sizeof(url), target->version, specs[i].release_name);
        if (err == CUP_OK) {
            err = download_file(url, paths[i], i == CUP_GENERATION_ASSET_BINARY
                                               ? DOWNLOAD_VALIDATE_BINARY
                                               : DOWNLOAD_VALIDATE_METADATA);
        }
        if (err == CUP_OK) err = verify_file_digest(paths[i], asset->sha256);
    }
#if !defined(_WIN32)
    if (err == CUP_OK) err = system_set_executable(files->binary, 1);
#endif
    return err;
}

CupError self_update_start(void) {
    CommandContext context;
    UpdateFiles files;
    ReleaseMetadata latest;
    ReleaseMetadata target;
    UpdateJournal journal;
    char target_release_sha256[65];
    char root[MAX_PATH_LEN];
    char helper_token[MAX_TRANSACTION_TOKEN_LEN];
    CupError err;
    int update_available = 0;
    int journal_started = 0;

    memset(&context, 0, sizeof(context));
    memset(&files, 0, sizeof(files));
    release_metadata_init(&latest);
    release_metadata_init(&target);
    update_journal_init(&journal);

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) err = layout_get_root(root, sizeof(root));
    if (err == CUP_OK) err = require_trusted_current_generation();
    if (err == CUP_OK) err = prepare_update_files(&files);
    if (err == CUP_OK) err = discover_target(&files, &latest, &update_available);
    if (err == CUP_OK && update_available) {
        printf("==> Downloading CUP %s (installed: %s)...\n", latest.version, CUP_VERSION_BASE);
        err = fetch_versioned_target(&files, &latest, &target);
    }
    if (err == CUP_OK && update_available) {
        err = update_generation_prepare(files.staging, target_release_sha256);
    }
    if (err == CUP_OK && update_available) {
        GenerationAssetSpec binary_spec;
        err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &binary_spec);
        if (err == CUP_OK) err = update_helper_prepare_from(binary_spec.destination);
    }
    if (err == CUP_OK && update_available) {
        err = text_format(helper_token, sizeof(helper_token), "u%lu-%s",
                          system_get_process_id(), path_last_segment(files.staging));
    }
    if (err == CUP_OK && update_available) {
        err = update_journal_begin(files.staging, target_release_sha256, &journal);
        if (err == CUP_OK || (err == CUP_ERR_COMMIT && journal.file_identity.valid)) {
            journal_started = 1;
        }
    }
    if (err == CUP_OK && update_available) err = interrupt_safe_point();
    if (err == CUP_OK && update_available) err = update_helper_start(root, helper_token, &context.lock);
    if (err == CUP_OK && update_available) {
        printf("Verified CUP update handoff accepted for %s. The generation will be committed after this process exits.\n",
               target.version);
    }

    /* Before helper ownership the parent can safely cancel the untouched generation transaction. */
    if (context.lock.active && files.staging[0] != '\0') {
        CupError cleanup_err = CUP_OK;
        if (journal_started) cleanup_err = runtime_journal_clear_if_identity(&journal.file_identity);
        if (cleanup_err == CUP_OK) cleanup_err = filesystem_remove_tree(files.staging);
        if (cleanup_err != CUP_OK) err = CUP_ERR_TRANSACTION;
        else if (err == CUP_ERR_COMMIT) err = CUP_ERR_TRANSACTION;
    }

    release_metadata_free(&target);
    release_metadata_free(&latest);
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
