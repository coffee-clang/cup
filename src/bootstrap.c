/* Verify one concrete release subset, then install it either by private-root publication
 * or by a synchronous generation transaction on an existing authenticated root. */

#include "bootstrap.h"

#include "checksum.h"
#include "constants.h"
#include "filesystem.h"
#include "generation.h"
#include "interrupt.h"
#include "layout.h"
#include "package_catalog.h"
#include "path.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "runtime_recovery.h"
#include "state.h"
#include "system.h"
#include "text.h"
#include "update_journal.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char binary_name[MAX_PATH_SEGMENT_LEN];
    char binary[MAX_PATH_LEN];
    char release[MAX_PATH_LEN];
    char license[MAX_PATH_LEN];
    char notices[MAX_PATH_LEN];
    char catalog[MAX_PATH_LEN];
    ReleaseMetadata metadata;
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

static void bootstrap_source_init(BootstrapSource *source) {
    if (source != NULL) {
        memset(source, 0, sizeof(*source));
        release_metadata_init(&source->metadata);
    }
}

static void bootstrap_source_free(BootstrapSource *source) {
    if (source != NULL) {
        release_metadata_free(&source->metadata);
        memset(source, 0, sizeof(*source));
    }
}

static void source_assets(BootstrapSource *source, BootstrapSourceAsset assets[5]) {
    assets[0] = (BootstrapSourceAsset){
        source->binary_name, source->binary, sizeof(source->binary)};
    assets[1] = (BootstrapSourceAsset){
        CUP_RELEASE_METADATA_FILENAME, source->release, sizeof(source->release)};
    assets[2] = (BootstrapSourceAsset){"LICENSE", source->license, sizeof(source->license)};
    assets[3] = (BootstrapSourceAsset){
        "THIRD_PARTY_NOTICES.txt", source->notices, sizeof(source->notices)};
    assets[4] = (BootstrapSourceAsset){
        CUP_CATALOG_FILENAME, source->catalog, sizeof(source->catalog)};
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

static CupError source_path(const char *directory,
                            const char *name,
                            char *path,
                            size_t size) {
    SystemPathKind kind;
    CupError err = path_join(path, size, directory, name);
    if (err == CUP_OK) err = system_get_path_kind(path, &kind);
    if (err != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) {
        return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

static CupError verify_manifest_file(const ReleaseMetadata *metadata,
                                     const char *name,
                                     const char *path) {
    const ReleaseAsset *asset;
    char digest[65];
    CupError err;

    if (metadata == NULL || text_is_empty(name) || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    asset = release_metadata_find_asset(metadata, name);
    if (asset == NULL) return CUP_ERR_VALIDATION;
    err = checksum_sha256_file(path, digest, sizeof(digest));
    return err == CUP_OK && strcmp(digest, asset->sha256) == 0
               ? CUP_OK
               : err != CUP_OK ? err : CUP_ERR_VALIDATION;
}

static CupError initialize_source(const char *directory, BootstrapSource *source) {
    BootstrapSourceAsset assets[5];
    BootstrapSourceSet set;
    int is_private = 0;
    CupError err;
    size_t i;

    if (text_is_empty(directory) || source == NULL) return CUP_ERR_INVALID_INPUT;
    bootstrap_source_init(source);
    err = system_directory_is_private(directory, &is_private);
    if (err != CUP_OK || !is_private) return err != CUP_OK ? err : CUP_ERR_VALIDATION;
    err = generation_binary_release_name(source->binary_name, sizeof(source->binary_name));
    if (err != CUP_OK) return err;
    source_assets(source, assets);
    for (i = 0; i < 5 && err == CUP_OK; ++i) {
        err = source_path(directory, assets[i].name, assets[i].path, assets[i].path_size);
    }
    if (err != CUP_OK) return err;
    set.assets = assets;
    set.count = 5;
    set.seen = 0;
    err = system_list_directory(directory, source_set_entry, &set);
    return err == CUP_OK && set.seen == set.count ? CUP_OK : CUP_ERR_VALIDATION;
}

static CupError verify_source(BootstrapSource *source, const char *running_binary) {
    PackageCatalog catalog;
    char source_digest[65];
    char running_digest[65];
    CupError err;

    if (source == NULL || text_is_empty(running_binary)) return CUP_ERR_INVALID_INPUT;
    package_catalog_init(&catalog);
    err = release_metadata_load(source->release, &source->metadata);
    if (err == CUP_OK) err = generation_validate_manifest(&source->metadata);
    if (err == CUP_OK && strcmp(source->metadata.version, CUP_VERSION_BASE) != 0) {
        err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK) err = verify_manifest_file(&source->metadata, source->binary_name, source->binary);
    if (err == CUP_OK) err = verify_manifest_file(&source->metadata, "LICENSE", source->license);
    if (err == CUP_OK) {
        err = verify_manifest_file(
            &source->metadata, "THIRD_PARTY_NOTICES.txt", source->notices);
    }
    if (err == CUP_OK) {
        err = verify_manifest_file(
            &source->metadata, CUP_CATALOG_FILENAME, source->catalog);
    }
    if (err == CUP_OK) err = package_catalog_load_path(&catalog, source->catalog);
    if (err == CUP_OK) err = checksum_sha256_file(source->binary, source_digest, sizeof(source_digest));
    if (err == CUP_OK) err = checksum_sha256_file(running_binary, running_digest, sizeof(running_digest));
    if (err == CUP_OK && strcmp(source_digest, running_digest) != 0) err = CUP_ERR_VALIDATION;
    package_catalog_free(&catalog);
    return err;
}

static CupError load_verified_source(const char *directory,
                                     const char *running_binary,
                                     BootstrapSource *source) {
    CupError err = initialize_source(directory, source);
    if (err == CUP_OK) err = verify_source(source, running_binary);
    if (err != CUP_OK) bootstrap_source_free(source);
    return err;
}

static CupError copy_file_with_permissions(const char *source,
                                           const char *destination,
                                           int executable,
                                           int read_only) {
    CupError err = system_copy_file(source, destination);
    if (err == CUP_OK) {
        err = filesystem_apply_required_permissions(destination, executable, read_only);
    }
    return err;
}

static const char *source_generation_path(const BootstrapSource *source, GenerationAssetId id) {
    switch (id) {
        case CUP_GENERATION_ASSET_RELEASE: return source->release;
        case CUP_GENERATION_ASSET_LICENSE: return source->license;
        case CUP_GENERATION_ASSET_NOTICES: return source->notices;
        case CUP_GENERATION_ASSET_BINARY: return source->binary;
        default: return NULL;
    }
}

static CupError install_source_generation(const BootstrapSource *source) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    CupError err = generation_asset_specs(specs);
    size_t i;

    for (i = 0; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        const char *from = source_generation_path(source, (GenerationAssetId)i);
        if (from == NULL) return CUP_ERR_INVALID_INPUT;
        err = copy_file_with_permissions(
            from, specs[i].destination, specs[i].executable, specs[i].read_only);
    }
    return err;
}

static CupError copy_source_generation_to_new(const BootstrapSource *source, const char *new_dir) {
    GenerationAssetId id;
    CupError err = CUP_OK;

    for (id = CUP_GENERATION_ASSET_RELEASE; err == CUP_OK && id < CUP_GENERATION_ASSET_COUNT; ++id) {
        char name[MAX_PATH_SEGMENT_LEN];
        char destination[MAX_PATH_LEN];
        const char *from = source_generation_path(source, id);
        err = generation_asset_release_name(id, name, sizeof(name));
        if (err == CUP_OK) err = path_join(destination, sizeof(destination), new_dir, name);
        if (err == CUP_OK) err = copy_file_with_permissions(
            from, destination, id == CUP_GENERATION_ASSET_BINARY, id != CUP_GENERATION_ASSET_BINARY);
    }
    return err;
}

static CupError ensure_empty_state(void) {
    CupState state;
    StateFileStatus status;
    CupError err;

    state_init(&state);
    err = state_load(&state, &status, NULL, stderr);
    if (err == CUP_OK && status == STATE_FILE_MISSING) {
        err = state_save(&state, NULL, NULL);
    }
    if (err == CUP_OK) err = state_validate(&state, stderr);
    state_free(&state);
    return err;
}

static CupError install_catalog_seed(const BootstrapSource *source, int preserve_invalid) {
    PackageCatalog local;
    char destination[MAX_PATH_LEN];
    SystemPathKind kind;
    SystemPathIdentity identity;
    char backup[MAX_PATH_LEN];
    CupError err;

    package_catalog_init(&local);
    memset(&identity, 0, sizeof(identity));
    err = layout_ensure_config();
    if (err == CUP_OK) err = layout_get_package_catalog_path(destination, sizeof(destination));
    if (err == CUP_OK) err = system_get_path_kind(destination, &kind);
    if (err != CUP_OK) goto done;
    if (kind == SYSTEM_PATH_REGULAR_FILE) {
        err = package_catalog_load_path(&local, destination);
        if (err == CUP_OK || err == CUP_ERR_NOT_AVAILABLE) goto done;
    } else if (kind == SYSTEM_PATH_MISSING) {
        err = system_copy_file(source->catalog, destination);
        goto done;
    }
    if (!preserve_invalid) {
        err = CUP_ERR_CATALOG;
        goto done;
    }
    err = system_get_path_identity(destination, &identity);
    if (err == CUP_OK) {
        err = filesystem_backup_invalid_if_identity(
            destination, &identity, backup, sizeof(backup));
    }
    if (err == CUP_OK) {
        printf("Preserved invalid catalog as '%s'.\n", backup);
        err = system_copy_file(source->catalog, destination);
    }

done:
    package_catalog_free(&local);
    return err;
}

static int release_version_compare(const ReleaseVersion *left, const ReleaseVersion *right) {
    if (left->major != right->major) return left->major < right->major ? -1 : 1;
    if (left->minor != right->minor) return left->minor < right->minor ? -1 : 1;
    if (left->patch != right->patch) return left->patch < right->patch ? -1 : 1;
    return 0;
}

static CupError reject_healthy_downgrade(const BootstrapSource *source) {
    GenerationAssetSpec release_spec;
    ReleaseMetadata current;
    ReleaseVersion current_version;
    ReleaseVersion target_version;
    SystemPathKind kind;
    CupError err;

    release_metadata_init(&current);
    err = generation_asset_spec(CUP_GENERATION_ASSET_RELEASE, &release_spec);
    if (err == CUP_OK) err = system_get_path_kind(release_spec.destination, &kind);
    if (err != CUP_OK || kind != SYSTEM_PATH_REGULAR_FILE) return err;
    err = release_metadata_load(release_spec.destination, &current);
    if (err != CUP_OK || generation_validate_manifest(&current) != CUP_OK) {
        release_metadata_free(&current);
        return CUP_OK;
    }
    err = release_version_parse(current.version, &current_version);
    if (err == CUP_OK) err = release_version_parse(source->metadata.version, &target_version);
    if (err == CUP_OK && release_version_compare(&current_version, &target_version) > 0) {
        fprintf(stderr,
                "Error: installed cup %s is newer than installer target %s; downgrade refused.\n",
                current.version,
                source->metadata.version);
        err = CUP_ERR_VALIDATION;
    }
    release_metadata_free(&current);
    return err;
}

static CupError build_fresh_root(const BootstrapSource *source,
                                 const char *base,
                                 const char *expected_root,
                                 char *published_root,
                                 size_t published_size) {
    char private_root[MAX_PATH_LEN];
    char selected[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    SystemLock lock;
    SystemPathKind kind;
    SystemPathIdentity private_identity;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    int snapshot_active = 0;
    int lock_active = 0;

    memset(&lock, 0, sizeof(lock));
    memset(&private_identity, 0, sizeof(private_identity));
    err = system_create_temp_directory(
        base, CUP_INSTALL_TEMP_PREFIX, private_root, sizeof(private_root));
    if (err == CUP_OK) {
        err = layout_root_snapshot_begin_private_at(private_root);
        if (err == CUP_OK) snapshot_active = 1;
    }
    if (err == CUP_OK) err = layout_ensure_root();
    if (err == CUP_OK) err = layout_ensure_runtime();
    if (err == CUP_OK) err = layout_ensure_assets();
    if (err == CUP_OK) err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) {
        err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) lock_active = 1;
    }
    if (err == CUP_OK) err = ensure_empty_state();
    if (err == CUP_OK) err = install_catalog_seed(source, 0);
    if (err == CUP_OK) err = install_source_generation(source);
    if (err == CUP_OK) err = layout_root_snapshot_validate();
    if (err == CUP_OK) err = system_get_path_identity(private_root, &private_identity);

    if (lock_active) system_lock_release(&lock);
    if (snapshot_active) layout_root_snapshot_end();

    if (err == CUP_OK) err = interrupt_safe_point();
    if (err == CUP_OK) err = layout_select_root_for_base(base, selected, sizeof(selected));
    if (err == CUP_OK && strcmp(selected, expected_root) != 0) err = CUP_ERR_INCONSISTENT_STATE;
    if (err == CUP_OK) err = system_get_path_kind(expected_root, &kind);
    if (err == CUP_OK && kind != SYSTEM_PATH_MISSING) err = CUP_ERR_INCONSISTENT_STATE;
    if (err == CUP_OK) {
        err = system_move_path_if_identity(
            private_root, expected_root, &private_identity, &commit_state);
        if (err == CUP_OK && commit_state != SYSTEM_COMMIT_DURABLE) err = CUP_ERR_COMMIT;
    }
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        (void)filesystem_remove_tree(private_root);
    }
    if (err == CUP_OK) err = text_copy(published_root, published_size, expected_root);
    return err;
}

static CupError replace_existing_generation(const BootstrapSource *source,
                                            const char *root,
                                            char *published_root,
                                            size_t published_size) {
    char lock_path[MAX_PATH_LEN];
    char staging_root[MAX_PATH_LEN];
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    char target_release_sha256[65];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    SystemLock lock;
    UpdateJournal journal;
    CupError err;
    int snapshot_active = 0;
    int lock_active = 0;
    int staging_created = 0;
    int journal_started = 0;

    memset(&lock, 0, sizeof(lock));
    update_journal_init(&journal);
    err = layout_root_snapshot_begin_at(root);
    if (err == CUP_OK) snapshot_active = 1;
    if (err == CUP_OK) err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) {
        err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
        if (err == CUP_OK) lock_active = 1;
    }
    if (err == CUP_OK) err = layout_root_snapshot_validate();
    if (err == CUP_OK) {
        err = runtime_recover_pending(&lock, NULL);
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Error: existing cup root has an interrupted operation that could not be recovered safely.\n");
        }
    }
    if (err == CUP_OK) err = reject_healthy_downgrade(source);
    if (err == CUP_OK) err = layout_ensure_runtime();
    if (err == CUP_OK) err = ensure_empty_state();
    if (err == CUP_OK) err = layout_ensure_assets();
    if (err == CUP_OK) err = install_catalog_seed(source, 1);
    if (err == CUP_OK) err = layout_get_staging_dir(staging_root, sizeof(staging_root));
    if (err == CUP_OK) {
        err = system_create_temp_directory(
            staging_root, CUP_UPDATE_TEMP_PREFIX, staging, sizeof(staging));
        if (err == CUP_OK) staging_created = 1;
    }
    if (err == CUP_OK) err = path_join(new_dir, sizeof(new_dir), staging, CUP_UPDATE_NEW_DIRECTORY);
    if (err == CUP_OK) {
        err = system_create_private_directory(new_dir, &commit_state);
        if (err == CUP_OK && commit_state != SYSTEM_COMMIT_DURABLE) err = CUP_ERR_COMMIT;
    }
    if (err == CUP_OK) err = copy_source_generation_to_new(source, new_dir);
    if (err == CUP_OK) err = update_generation_prepare(staging, target_release_sha256);
    if (err == CUP_OK) {
        err = update_journal_begin(staging, target_release_sha256, &journal);
        if (err == CUP_OK || err == CUP_ERR_COMMIT) journal_started = 1;
    }
    if (err == CUP_OK) err = update_generation_commit(&journal);
    if (err == CUP_OK) {
        staging_created = 0;
        journal_started = 0;
        err = text_copy(published_root, published_size, root);
    }

    if (err != CUP_OK && staging_created && !journal_started) {
        (void)filesystem_remove_tree(staging);
    }
    if (lock_active) system_lock_release(&lock);
    if (snapshot_active) layout_root_snapshot_end();
    return err;
}

CupError bootstrap_start(const char *source_directory,
                         const char *running_binary,
                         const char *base) {
    BootstrapSource source;
    char selected_root[MAX_PATH_LEN];
    char published_root[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    if (text_is_empty(source_directory) || text_is_empty(running_binary) || text_is_empty(base)) {
        return CUP_ERR_INVALID_INPUT;
    }
    bootstrap_source_init(&source);
    err = load_verified_source(source_directory, running_binary, &source);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: bootstrap source release is invalid.\n");
        return err;
    }
    err = layout_select_root_for_base(base, selected_root, sizeof(selected_root));
    if (err == CUP_OK) err = system_get_path_kind(selected_root, &kind);
    if (err == CUP_OK && kind == SYSTEM_PATH_MISSING) {
        err = build_fresh_root(
            &source, base, selected_root, published_root, sizeof(published_root));
    } else if (err == CUP_OK && kind == SYSTEM_PATH_DIRECTORY) {
        err = replace_existing_generation(
            &source, selected_root, published_root, sizeof(published_root));
    } else if (err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }

    if (err == CUP_OK) {
        printf("CUP_BOOTSTRAP_ROOT=%s\n", published_root);
        printf("Verified cup %s generation installed.\n", source.metadata.version);
    } else {
        fprintf(stderr, "Error: verified cup generation could not be installed safely.\n");
    }
    bootstrap_source_free(&source);
    return err;
}
