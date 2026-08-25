/*
 * Validates one complete release generation before creating managed runtime state, then stages the
 * installed release assets and delegates the atomic post-exit commit to the canonical update
 * helper.
 */

#include "bootstrap.h"

#include "checksum.h"
#include "constants.h"
#include "assets.h"
#include "update_helper.h"
#include "update_journal.h"
#include "filesystem.h"
#include "install_policy.h"
#include "interrupt.h"
#include "layout.h"
#include "package_catalog.h"
#include "path.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "state.h"
#include "system.h"
#include "text.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char binary_name[MAX_IDENTIFIER_LEN];
    char platform_checksums_name[MAX_IDENTIFIER_LEN];
    char binary[MAX_PATH_LEN];
    char release[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    char catalog[MAX_PATH_LEN];
    char install_policy[MAX_PATH_LEN];
    char install_posix[MAX_PATH_LEN];
    char install_windows[MAX_PATH_LEN];
} BootstrapSource;

typedef struct {
    const char *name;
    char *path;
    size_t path_size;
} BootstrapSourceAsset;

typedef struct {
    const BootstrapSourceAsset *assets;
    size_t count;
    size_t seen;
} BootstrapSourceSet;

static void source_assets(BootstrapSource *source, BootstrapSourceAsset assets[8]) {
    assets[0] = (BootstrapSourceAsset){
        source->binary_name, source->binary, sizeof(source->binary)};
    assets[1] = (BootstrapSourceAsset){
        CUP_RELEASE_METADATA_FILENAME, source->release, sizeof(source->release)};
    assets[2] = (BootstrapSourceAsset){source->platform_checksums_name,
                                       source->platform_checksums,
                                       sizeof(source->platform_checksums)};
    assets[3] = (BootstrapSourceAsset){CUP_COMMON_CHECKSUMS_FILENAME,
                                       source->common_checksums,
                                       sizeof(source->common_checksums)};
    assets[4] = (BootstrapSourceAsset){
        CUP_PACKAGES_FILENAME, source->catalog, sizeof(source->catalog)};
    assets[5] = (BootstrapSourceAsset){CUP_INSTALL_POLICY_FILENAME,
                                       source->install_policy,
                                       sizeof(source->install_policy)};
    assets[6] = (BootstrapSourceAsset){CUP_INSTALL_POSIX_FILENAME,
                                       source->install_posix,
                                       sizeof(source->install_posix)};
    assets[7] = (BootstrapSourceAsset){CUP_INSTALL_WINDOWS_FILENAME,
                                       source->install_windows,
                                       sizeof(source->install_windows)};
}

static int absolute_path_is_valid(const char *path) {
    if (text_is_empty(path)) {
        return 0;
    }
#if defined(_WIN32)
    return ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '/' || path[2] == '\\');
#else
    return path[0] == '/';
#endif
}

static CupError build_source_path(const char *directory,
                                  const char *name,
                                  char *path,
                                  size_t size) {
    CupError err = path_join(path, size, directory, name);
    SystemPathKind kind;

    if (err == CUP_OK) {
        err = system_get_path_kind(path, &kind);
    }
    if (err != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) {
        return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError source_set_entry(const char *entry,
                                 SystemPathKind kind,
                                 const SystemPathIdentity *identity,
                                 void *userdata) {
    BootstrapSourceSet *set = userdata;
    const char *name = path_last_segment(entry);
    size_t i;

    (void)identity;

    if (set == NULL || kind != SYSTEM_PATH_REGULAR_FILE || text_is_empty(name)) {
        return CUP_ERR_VALIDATION;
    }
    for (i = 0; i < set->count; ++i) {
        if (strcmp(name, set->assets[i].name) == 0) {
            set->seen++;
            return CUP_OK;
        }
    }
    return CUP_ERR_VALIDATION;
}

static CupError initialize_source(const char *directory, BootstrapSource *source) {
    BootstrapSourceAsset assets[8];
    BootstrapSourceSet set;
    CupError err;
    int is_private = 0;
    size_t i;

    if (!absolute_path_is_valid(directory) || source == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(source, 0, sizeof(*source));
    err = system_directory_is_private(directory, &is_private);
    if (err != CUP_OK || !is_private) {
        return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    }
    err = assets_binary_asset_name(source->binary_name, sizeof(source->binary_name));
    if (err == CUP_OK) {
        err = assets_platform_checksums_name(source->platform_checksums_name,
                                            sizeof(source->platform_checksums_name));
    }
    if (err != CUP_OK) {
        return err;
    }

    source_assets(source, assets);

    for (i = 0; i < sizeof(assets) / sizeof(assets[0]); ++i) {
        err = build_source_path(
            directory, assets[i].name, assets[i].path, assets[i].path_size);
        if (err != CUP_OK) {
            return err;
        }
    }

    set.assets = assets;
    set.count = sizeof(assets) / sizeof(assets[0]);
    set.seen = 0;
    err = system_list_directory(directory, source_set_entry, &set);
    return err == CUP_OK && set.seen == set.count ? CUP_OK : CUP_ERR_VALIDATION;
}

typedef struct {
    const ChecksumDocument *document;
    const char *name;
    const char *path;
} BootstrapChecksumAsset;

static CupError verify_file(const ChecksumDocument *document,
                            const char *name,
                            const char *path) {
    int matches = 0;
    CupError err = checksum_document_verify_file(document, name, path, &matches);

    return err == CUP_OK && matches ? CUP_OK : err != CUP_OK ? err : CUP_ERR_VALIDATION;
}

static CupError verify_source(const BootstrapSource *source, const char *running_binary) {
    const char *platform_assets[CUP_PLATFORM_CHECKSUM_ASSET_COUNT];
    ChecksumDocument platform_document;
    ChecksumDocument common_document;
    ReleaseMetadata metadata;
    PackageCatalog catalog;
    InstallPolicy policy;
    char source_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char running_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    CupError err;

    if (source == NULL || !absolute_path_is_valid(running_binary)) {
        return CUP_ERR_INVALID_INPUT;
    }
    checksum_document_init(&platform_document);
    checksum_document_init(&common_document);
    package_catalog_init(&catalog);
    install_policy_init(&policy);

    platform_assets[0] = source->binary_name;
    platform_assets[1] = CUP_RELEASE_METADATA_FILENAME;
    platform_assets[2] = CUP_COMMON_CHECKSUMS_FILENAME;
    err = checksum_document_load(&platform_document, source->platform_checksums);
    if (err == CUP_OK) {
        err = checksum_document_validate_assets(
            &platform_document, platform_assets, CUP_PLATFORM_CHECKSUM_ASSET_COUNT);
    }
    if (err == CUP_OK) {
        err = checksum_document_load(&common_document, source->common_checksums);
    }
    if (err == CUP_OK) {
        err = checksum_document_validate_assets(
            &common_document, CUP_COMMON_CHECKSUM_ASSETS, CUP_COMMON_CHECKSUM_ASSET_COUNT);
    }
    if (err == CUP_OK) {
        BootstrapChecksumAsset assets[] = {
            {&platform_document, CUP_COMMON_CHECKSUMS_FILENAME, source->common_checksums},
            {&platform_document, source->binary_name, source->binary},
            {&platform_document, CUP_RELEASE_METADATA_FILENAME, source->release},
            {&common_document, CUP_PACKAGES_FILENAME, source->catalog},
            {&common_document, CUP_INSTALL_POLICY_FILENAME, source->install_policy},
            {&common_document, CUP_INSTALL_POSIX_FILENAME, source->install_posix},
            {&common_document, CUP_INSTALL_WINDOWS_FILENAME, source->install_windows},
        };
        size_t i;

        for (i = 0; i < sizeof(assets) / sizeof(assets[0]) && err == CUP_OK; ++i) {
            err = verify_file(assets[i].document, assets[i].name, assets[i].path);
        }
    }
    if (err == CUP_OK) {
        err = release_metadata_load(source->release, &metadata);
    }
    if (err == CUP_OK && strcmp(metadata.version, CUP_VERSION_BASE) != 0) {
        err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK) {
        err = package_catalog_load_path(&catalog, source->catalog);
    }
    if (err == CUP_OK) {
        err = install_policy_load_path(&policy, source->install_policy);
    }
    if (err == CUP_OK) {
        err = checksum_sha256_file(source->binary, source_hash, sizeof(source_hash));
    }
    if (err == CUP_OK) {
        err = checksum_sha256_file(running_binary, running_hash, sizeof(running_hash));
    }
    if (err == CUP_OK && strcmp(source_hash, running_hash) != 0) {
        err = CUP_ERR_VALIDATION;
    }

    package_catalog_free(&catalog);
    checksum_document_free(&common_document);
    checksum_document_free(&platform_document);
    return err;
}

static CupError stage_copy(const char *staging,
                           const char *name,
                           const char *source,
                           char *destination,
                           size_t size) {
    CupError err = path_join(destination, size, staging, name);

    if (err == CUP_OK) {
        err = system_copy_file(source, destination);
    }
    return err;
}

static CupError load_verified_source(const char *directory,
                                     const char *running_binary,
                                     BootstrapSource *source) {
    CupError err = initialize_source(directory, source);

    return err == CUP_OK ? verify_source(source, running_binary) : err;
}

static CupError copy_source_generation(BootstrapSource *source, const char *staging) {
    BootstrapSourceAsset assets[8];
    char destination[MAX_PATH_LEN];
    CupError err = CUP_OK;
    size_t i;

    if (source == NULL || text_is_empty(staging)) {
        return CUP_ERR_INVALID_INPUT;
    }

    source_assets(source, assets);
    for (i = 0; i < sizeof(assets) / sizeof(assets[0]) && err == CUP_OK; ++i) {
        err = stage_copy(staging,
                         assets[i].name,
                         assets[i].path,
                         destination,
                         sizeof(destination));
    }
    return err;
}

static CupError ensure_bootstrap_state(void) {
    CupState state;
    StateFileStatus status;
    CupError err;

    memset(&state, 0, sizeof(state));
    err = state_load(&state, &status, NULL, stderr);
    if (err != CUP_OK) {
        return err;
    }
    return status == STATE_FILE_MISSING ? state_save(&state, NULL, NULL)
                                        : state_validate(&state, stderr);
}

static CupError stage_bootstrap_assets(const BootstrapSource *source,
                                       const char *staging,
                                       char *staged_binary,
                                       size_t binary_size) {
    char ignored[MAX_PATH_LEN];
    CupError err;

    err = stage_copy(
        staging, CUP_UPDATE_BINARY_NEW, source->binary, staged_binary, binary_size);
    if (err == CUP_OK) {
        err = stage_copy(staging,
                         CUP_UPDATE_PLATFORM_CHECKSUMS_NEW,
                         source->platform_checksums,
                         ignored,
                         sizeof(ignored));
    }
    if (err == CUP_OK) {
        err = stage_copy(staging,
                         CUP_UPDATE_PACKAGES_NEW,
                         source->catalog,
                         ignored,
                         sizeof(ignored));
    }
    if (err == CUP_OK) {
        err = stage_copy(staging,
                         CUP_UPDATE_INSTALL_POLICY_NEW,
                         source->install_policy,
                         ignored,
                         sizeof(ignored));
    }
    if (err == CUP_OK) {
        err = stage_copy(staging,
                         CUP_UPDATE_COMMON_CHECKSUMS_NEW,
                         source->common_checksums,
                         ignored,
                         sizeof(ignored));
    }
#if !defined(_WIN32)
    if (err == CUP_OK) {
        err = system_set_executable(staged_binary, 1);
    }
#endif
    return err;
}

typedef struct {
    BootstrapSource source;
    UpdateJournal journal;
    SystemLock lock;
    char staging[MAX_PATH_LEN];
    char staged_binary[MAX_PATH_LEN];
    char token[MAX_TRANSACTION_TOKEN_LEN];
    char root[MAX_PATH_LEN];
    int transaction_started;
} BootstrapOperation;

static CupError cleanup_failed_bootstrap(const char *staging,
                                         int transaction_started,
                                         const UpdateJournal *journal) {
    CupError err = CUP_OK;

    if (text_is_empty(staging)) {
        return CUP_OK;
    }
    if (transaction_started) {
        err = journal != NULL && journal->file_identity.valid
                  ? runtime_journal_clear_if_identity(&journal->file_identity)
                  : CUP_ERR_TRANSACTION;
    }
    return err == CUP_OK ? filesystem_remove_tree(staging) : err;
}

static CupError prepare_bootstrap_operation(BootstrapOperation *operation,
                                            const char *source_directory,
                                            const char *running_binary) {
    char lock_path[MAX_PATH_LEN];
    SystemPathKind root_kind;
    CupError err;

    memset(operation, 0, sizeof(*operation));
    update_journal_init(&operation->journal);

    err = load_verified_source(source_directory, running_binary, &operation->source);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: bootstrap source generation is invalid.\n");
        return err;
    }

    err = layout_get_root(operation->root, sizeof(operation->root));
    if (err == CUP_OK) {
        err = system_get_path_kind(operation->root, &root_kind);
    }
    if (err == CUP_OK && root_kind == SYSTEM_PATH_MISSING) {
        /* A missing root must be created before cup.lock can exist. Existing roots, however, are
         * not permission-mutated before the canonical exclusive lock. */
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = layout_ensure_root();
        }
    } else if (err == CUP_OK && root_kind != SYSTEM_PATH_DIRECTORY) {
        err = CUP_ERR_FILESYSTEM;
    }
    if (err == CUP_OK) {
        err = layout_get_lock_path(lock_path, sizeof(lock_path));
    }
    if (err == CUP_OK) {
        err = system_lock_acquire(&operation->lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    }
    if (err == CUP_OK) {
        err = layout_root_snapshot_validate();
    }
    if (err == CUP_OK) {
        err = layout_ensure_root();
    }
    if (err == CUP_OK) {
        err = runtime_journal_require_none();
    }
    if (err == CUP_OK) {
        err = layout_ensure_runtime();
    }
    if (err == CUP_OK) {
        err = ensure_bootstrap_state();
    }
    if (err == CUP_OK) {
        err = layout_ensure_assets();
    }
    if (err != CUP_OK) {
        fprintf(stderr, "Error: could not prepare bootstrap transaction.\n");
    }
    return err;
}

static CupError stage_bootstrap_generation(BootstrapOperation *operation,
                                           const char *running_binary) {
    BootstrapSource staged_source;
    char staging_root[MAX_PATH_LEN];
    CupError err;

    err = layout_get_staging_dir(staging_root, sizeof(staging_root));
    if (err == CUP_OK) {
        err = system_create_temp_directory(
            staging_root, "cup-update", operation->staging, sizeof(operation->staging));
    }
    if (err == CUP_OK) {
        err = copy_source_generation(&operation->source, operation->staging);
    }
    if (err == CUP_OK) {
        err = load_verified_source(operation->staging, running_binary, &staged_source);
    }
    if (err == CUP_OK) {
        operation->source = staged_source;
    }
    if (err == CUP_OK) {
        err = stage_bootstrap_assets(&operation->source,
                                     operation->staging,
                                     operation->staged_binary,
                                     sizeof(operation->staged_binary));
    }
    if (err == CUP_OK) {
        err = text_format(operation->token,
                          sizeof(operation->token),
                          "b%lu-%s",
                          system_get_process_id(),
                          path_last_segment(operation->staging));
    }
    if (err == CUP_OK) {
        err = update_helper_prepare_from(operation->source.binary);
    }
    if (err == CUP_OK) {
        err = update_journal_begin(operation->staging,
                                       operation->token,
                                       CUP_VERSION_BASE,
                                       &operation->journal);
    }
    if (err != CUP_OK) {
        return err;
    }

    operation->transaction_started = 1;
    return interrupt_safe_point();
}

CupError bootstrap_start(const char *source_directory, const char *running_binary) {
    BootstrapOperation operation;
    CupError err;

    err = prepare_bootstrap_operation(&operation, source_directory, running_binary);
    if (err == CUP_OK) {
        err = stage_bootstrap_generation(&operation, running_binary);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: could not stage verified bootstrap generation.\n");
        }
    }
    if (err == CUP_OK) {
        err = update_helper_start(operation.root, operation.token, &operation.lock);
        if (err != CUP_OK) {
            fprintf(stderr, "Error: could not start bootstrap update helper.\n");
        }
    }

    if (err == CUP_OK) {
        printf("CUP_BOOTSTRAP_ROOT=%s\n", operation.root);
        printf("Verified cup %s installation scheduled. The update helper will "
               "commit it after this process exits.\n",
               CUP_VERSION_BASE);
    } else {
        CupError cleanup_err = cleanup_failed_bootstrap(operation.staging,
                                                        operation.transaction_started,
                                                        &operation.journal);

        if (cleanup_err != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
        }
    }

    system_lock_release(&operation.lock);
    return err;
}
