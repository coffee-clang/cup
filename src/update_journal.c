/* Minimal CUP-generation transaction: complete new/old evidence plus binary-last commit. */

#include "update_journal.h"

#include "checksum.h"
#include "filesystem.h"
#include "generation.h"
#include "layout.h"
#include "path.h"
#include "release_metadata.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define CUP_UPDATE_JOURNAL_FORMAT "2"

static int temporary_name_is_valid(const char *name) {
    return !text_is_empty(name) && strlen(name) < MAX_METADATA_VALUE_LEN &&
           path_generated_temp_suffix(name, CUP_UPDATE_TEMP_PREFIX) != NULL;
}

void update_journal_init(UpdateJournal *journal) {
    if (journal != NULL) memset(journal, 0, sizeof(*journal));
}

static CupError write_journal(FILE *file, const void *value) {
    const UpdateJournal *journal = value;
    if (journal == NULL ||
        fprintf(file, "format=%s\n", CUP_UPDATE_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=%s\n", CUP_UPDATE_JOURNAL_OPERATION) < 0 ||
        fprintf(file, "target_release_sha256=%s\n", journal->target_release_sha256) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

CupError update_journal_begin(const char *temporary_path,
                              const char *target_release_sha256,
                              UpdateJournal *created) {
    UpdateJournal journal;
    SystemPathIdentity identity;
    char staging_dir[MAX_PATH_LEN];
    const char *name;
    CupError err;

    if (created == NULL || text_is_empty(temporary_path) ||
        !checksum_digest_is_canonical(target_release_sha256)) {
        return CUP_ERR_INVALID_INPUT;
    }
    update_journal_init(created);
    name = path_last_segment(temporary_path);
    if (!temporary_name_is_valid(name)) return CUP_ERR_INVALID_INPUT;

    update_journal_init(&journal);
    if (text_copy(journal.temporary_name, sizeof(journal.temporary_name), name) != CUP_OK ||
        text_copy(journal.target_release_sha256, sizeof(journal.target_release_sha256),
                  target_release_sha256) != CUP_OK ||
        layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    memset(&identity, 0, sizeof(identity));
    err = runtime_journal_publish(staging_dir, "transaction", NULL, write_journal, &journal, &identity);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && identity.valid) {
        journal.file_identity = identity;
        *created = journal;
    }
    return err;
}

typedef struct {
    UpdateJournal *journal;
} ParseContext;

static CupError parse_field(const char *key, const char *value, void *userdata) {
    ParseContext *context = userdata;
    UpdateJournal *journal;
    if (context == NULL || context->journal == NULL) return CUP_ERR_TRANSACTION;
    journal = context->journal;
    if (strcmp(key, "format") == 0) {
        return strcmp(value, CUP_UPDATE_JOURNAL_FORMAT) == 0 ? CUP_OK : CUP_ERR_TRANSACTION;
    }
    if (strcmp(key, "operation") == 0) {
        return strcmp(value, CUP_UPDATE_JOURNAL_OPERATION) == 0 ? CUP_OK : CUP_ERR_TRANSACTION;
    }
    if (strcmp(key, "target_release_sha256") == 0) {
        if (!checksum_digest_is_canonical(value) ||
            text_copy(journal->target_release_sha256,
                      sizeof(journal->target_release_sha256), value) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        return CUP_OK;
    }
    if (strcmp(key, "temporary_name") == 0) {
        if (!temporary_name_is_valid(value) ||
            text_copy(journal->temporary_name, sizeof(journal->temporary_name), value) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        return CUP_OK;
    }
    return CUP_ERR_TRANSACTION;
}

CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status) {
    static const char *const keys[] = {
        "format", "operation", "target_release_sha256", "temporary_name"};
    UpdateJournal candidate;
    ParseContext context;
    SystemPathIdentity identity;
    CupError err;
    int missing;

    if (journal == NULL || status == NULL) return CUP_ERR_INVALID_INPUT;
    update_journal_init(journal);
    update_journal_init(&candidate);
    memset(&identity, 0, sizeof(identity));
    *status = CUP_UPDATE_JOURNAL_MISSING;
    context.journal = &candidate;
    err = runtime_journal_parse(keys, sizeof(keys) / sizeof(keys[0]),
                                parse_field, &context, &identity, &missing);
    if (err != CUP_OK || missing) return err;
    if (!temporary_name_is_valid(candidate.temporary_name) ||
        !checksum_digest_is_canonical(candidate.target_release_sha256)) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.file_identity = identity;
    *journal = candidate;
    *status = CUP_UPDATE_JOURNAL_LOADED;
    return CUP_OK;
}

static CupError update_journal_get_staging_path(const UpdateJournal *journal,
                                         char *buffer,
                                         size_t size) {
    char staging[MAX_PATH_LEN];
    if (journal == NULL || buffer == NULL || size == 0 ||
        !temporary_name_is_valid(journal->temporary_name)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_staging_dir(staging, sizeof(staging)) != CUP_OK) return CUP_ERR_TRANSACTION;
    return path_join(buffer, size, staging, journal->temporary_name);
}

static CupError workspace_child(char *path, size_t size, const char *workspace, const char *child) {
    if (text_is_empty(workspace) || !path_is_safe_segment(child)) return CUP_ERR_INVALID_INPUT;
    return path_join(path, size, workspace, child);
}

static CupError generation_source_path(char *path,
                                       size_t size,
                                       const char *directory,
                                       GenerationAssetId id) {
    GenerationAssetSpec spec;
    CupError err = generation_asset_spec(id, &spec);
    return err == CUP_OK ? path_join(path, size, directory, spec.release_name) : err;
}

static CupError require_regular_or_missing(const char *path, SystemPathKind *kind) {
    CupError err = system_get_path_kind(path, kind);
    if (err != CUP_OK) return err;
    return (*kind == SYSTEM_PATH_REGULAR_FILE || *kind == SYSTEM_PATH_MISSING)
               ? CUP_OK : CUP_ERR_TRANSACTION;
}

typedef struct {
    const char *names[CUP_GENERATION_ASSET_COUNT];
    size_t seen;
} ExactGenerationSet;

static CupError exact_generation_entry(const char *entry,
                                       SystemPathKind kind,
                                       const SystemPathIdentity *identity,
                                       void *userdata) {
    ExactGenerationSet *set = userdata;
    const char *name = path_last_segment(entry);
    size_t i;
    (void)identity;
    if (set == NULL || kind != SYSTEM_PATH_REGULAR_FILE || text_is_empty(name)) {
        return CUP_ERR_VALIDATION;
    }
    for (i = 0; i < CUP_GENERATION_ASSET_COUNT; ++i) {
        if (strcmp(name, set->names[i]) == 0) {
            set->seen++;
            return CUP_OK;
        }
    }
    return CUP_ERR_VALIDATION;
}

static CupError validate_new_generation(const char *new_dir,
                                        ReleaseMetadata *metadata,
                                        char release_sha256[65]) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    ExactGenerationSet set;
    char release_path[MAX_PATH_LEN];
    CupError err;
    size_t i;

    if (metadata == NULL || release_sha256 == NULL) return CUP_ERR_INVALID_INPUT;
    release_metadata_init(metadata);
    memset(&set, 0, sizeof(set));
    err = generation_asset_specs(specs);
    for (i = 0; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        char path[MAX_PATH_LEN];
        SystemPathKind kind;
        set.names[i] = specs[i].release_name;
        err = path_join(path, sizeof(path), new_dir, specs[i].release_name);
        if (err == CUP_OK) err = system_get_path_kind(path, &kind);
        if (err == CUP_OK && kind != SYSTEM_PATH_REGULAR_FILE) err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK) {
        err = system_list_directory(new_dir, exact_generation_entry, &set);
    }
    if (err == CUP_OK && set.seen != CUP_GENERATION_ASSET_COUNT) err = CUP_ERR_VALIDATION;
    if (err == CUP_OK) {
        err = generation_source_path(release_path, sizeof(release_path), new_dir,
                                     CUP_GENERATION_ASSET_RELEASE);
    }
    if (err == CUP_OK) err = release_metadata_load(release_path, metadata);
    if (err == CUP_OK) err = generation_validate_manifest(metadata);
    for (i = CUP_GENERATION_ASSET_LICENSE; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        char path[MAX_PATH_LEN];
        char digest[65];
        const ReleaseAsset *asset = generation_manifest_asset(metadata, (GenerationAssetId)i);
        if (asset == NULL) { err = CUP_ERR_VALIDATION; break; }
        err = generation_source_path(path, sizeof(path), new_dir, (GenerationAssetId)i);
        if (err == CUP_OK) err = checksum_sha256_file(path, digest, sizeof(digest));
        if (err == CUP_OK && strcmp(digest, asset->sha256) != 0) err = CUP_ERR_VALIDATION;
    }
    if (err == CUP_OK) err = checksum_sha256_file(release_path, release_sha256, 65);
    if (err != CUP_OK) release_metadata_free(metadata);
    return err;
}

static CupError snapshot_old_generation(const char *old_dir) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err = generation_asset_specs(specs);
    size_t i;

    if (err == CUP_OK) err = system_create_private_directory(old_dir, &commit_state);
    if (err != CUP_OK) return err;
    for (i = 0; i < CUP_GENERATION_ASSET_COUNT; ++i) {
        char target[MAX_PATH_LEN];
        SystemPathKind kind;
        err = require_regular_or_missing(specs[i].destination, &kind);
        if (err != CUP_OK) return err;
        if (kind == SYSTEM_PATH_MISSING) continue;
        err = path_join(target, sizeof(target), old_dir, specs[i].release_name);
        if (err == CUP_OK) err = system_copy_file(specs[i].destination, target);
        if (err != CUP_OK) return err;
    }
    return CUP_OK;
}

CupError update_generation_prepare(const char *staging,
                                   char target_release_sha256[65]) {
    char new_dir[MAX_PATH_LEN];
    char old_dir[MAX_PATH_LEN];
    ReleaseMetadata metadata;
    SystemPathKind old_kind;
    CupError err;

    if (text_is_empty(staging) || target_release_sha256 == NULL) return CUP_ERR_INVALID_INPUT;
    err = workspace_child(new_dir, sizeof(new_dir), staging, CUP_UPDATE_NEW_DIRECTORY);
    if (err == CUP_OK) err = workspace_child(old_dir, sizeof(old_dir), staging, CUP_UPDATE_OLD_DIRECTORY);
    if (err == CUP_OK) err = system_get_path_kind(old_dir, &old_kind);
    if (err == CUP_OK && old_kind != SYSTEM_PATH_MISSING) err = CUP_ERR_TRANSACTION;
    if (err == CUP_OK) err = validate_new_generation(new_dir, &metadata, target_release_sha256);
    if (err == CUP_OK) release_metadata_free(&metadata);
    if (err == CUP_OK) err = snapshot_old_generation(old_dir);
    return err;
}

static CupError files_match(const char *left, const char *right, int *matches) {
    char left_digest[65];
    char right_digest[65];
    CupError err;
    if (matches == NULL) return CUP_ERR_INVALID_INPUT;
    *matches = 0;
    err = checksum_sha256_file(left, left_digest, sizeof(left_digest));
    if (err == CUP_OK) err = checksum_sha256_file(right, right_digest, sizeof(right_digest));
    if (err == CUP_OK) *matches = strcmp(left_digest, right_digest) == 0;
    return err;
}

static CupError canonical_matches_snapshot(const char *directory, int *matches) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    CupError err = generation_asset_specs(specs);
    size_t i;
    if (matches == NULL) return CUP_ERR_INVALID_INPUT;
    *matches = 0;
    for (i = 0; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        char snapshot[MAX_PATH_LEN];
        SystemPathKind snapshot_kind;
        SystemPathKind canonical_kind;
        int equal;
        err = path_join(snapshot, sizeof(snapshot), directory, specs[i].release_name);
        if (err == CUP_OK) err = require_regular_or_missing(snapshot, &snapshot_kind);
        if (err == CUP_OK) err = require_regular_or_missing(specs[i].destination, &canonical_kind);
        if (err != CUP_OK) break;
        if (snapshot_kind != canonical_kind) return CUP_OK;
        if (snapshot_kind == SYSTEM_PATH_REGULAR_FILE) {
            err = files_match(snapshot, specs[i].destination, &equal);
            if (err != CUP_OK) break;
            if (!equal) return CUP_OK;
        }
    }
    if (err == CUP_OK) *matches = 1;
    return err;
}

static CupError canonical_binary_matches_old(const char *old_dir, int *matches) {
    GenerationAssetSpec spec;
    char old_path[MAX_PATH_LEN];
    SystemPathKind old_kind;
    SystemPathKind current_kind;
    CupError err = generation_asset_spec(CUP_GENERATION_ASSET_BINARY, &spec);
    int equal;
    if (matches == NULL) return CUP_ERR_INVALID_INPUT;
    *matches = 0;
    if (err == CUP_OK) err = path_join(old_path, sizeof(old_path), old_dir, spec.release_name);
    if (err == CUP_OK) err = require_regular_or_missing(old_path, &old_kind);
    if (err == CUP_OK) err = require_regular_or_missing(spec.destination, &current_kind);
    if (err != CUP_OK || old_kind != current_kind) return err;
    if (old_kind == SYSTEM_PATH_MISSING) { *matches = 1; return CUP_OK; }
    err = files_match(old_path, spec.destination, &equal);
    if (err == CUP_OK) *matches = equal;
    return err;
}

static CupError target_matches_installed(const char *new_dir,
                                         const char *target_release_sha256,
                                         int *matches) {
    GenerationAssetSpec specs[CUP_GENERATION_ASSET_COUNT];
    ReleaseMetadata metadata;
    char release_digest[65];
    char release_path[MAX_PATH_LEN];
    CupError err;
    size_t i;
    if (matches == NULL) return CUP_ERR_INVALID_INPUT;
    *matches = 0;
    err = validate_new_generation(new_dir, &metadata, release_digest);
    if (err != CUP_OK) return err;
    if (strcmp(release_digest, target_release_sha256) != 0) {
        release_metadata_free(&metadata);
        return CUP_ERR_TRANSACTION;
    }
    err = generation_asset_specs(specs);
    if (err == CUP_OK) err = generation_source_path(release_path, sizeof(release_path), new_dir,
                                                     CUP_GENERATION_ASSET_RELEASE);
    if (err == CUP_OK) {
        SystemPathKind kind;
        int equal;
        err = require_regular_or_missing(specs[CUP_GENERATION_ASSET_RELEASE].destination, &kind);
        if (err == CUP_OK && kind != SYSTEM_PATH_REGULAR_FILE) { release_metadata_free(&metadata); return CUP_OK; }
        if (err == CUP_OK) err = files_match(release_path, specs[CUP_GENERATION_ASSET_RELEASE].destination, &equal);
        if (err == CUP_OK && !equal) { release_metadata_free(&metadata); return CUP_OK; }
    }
    for (i = CUP_GENERATION_ASSET_LICENSE; err == CUP_OK && i < CUP_GENERATION_ASSET_COUNT; ++i) {
        const ReleaseAsset *asset = generation_manifest_asset(&metadata, (GenerationAssetId)i);
        char digest[65];
        SystemPathKind kind;
        err = require_regular_or_missing(specs[i].destination, &kind);
        if (err == CUP_OK && kind != SYSTEM_PATH_REGULAR_FILE) { release_metadata_free(&metadata); return CUP_OK; }
        if (err == CUP_OK) err = checksum_sha256_file(specs[i].destination, digest, sizeof(digest));
        if (err == CUP_OK && (asset == NULL || strcmp(digest, asset->sha256) != 0)) {
            release_metadata_free(&metadata);
            return CUP_OK;
        }
    }
    release_metadata_free(&metadata);
    if (err == CUP_OK) *matches = 1;
    return err;
}

static CupError prepare_destination_for_replace(const GenerationAssetSpec *spec) {
    SystemPathKind kind;
    CupError err = require_regular_or_missing(spec->destination, &kind);
    if (err == CUP_OK && kind == SYSTEM_PATH_REGULAR_FILE && spec->read_only) {
        err = system_set_read_only(spec->destination, 0);
    }
    return err;
}

static CupError install_new_asset(const char *new_dir, GenerationAssetId id) {
    GenerationAssetSpec spec;
    char source[MAX_PATH_LEN];
    CupError err = generation_asset_spec(id, &spec);
    if (err == CUP_OK) err = generation_source_path(source, sizeof(source), new_dir, id);
    if (err == CUP_OK) err = prepare_destination_for_replace(&spec);
    if (err == CUP_OK) err = system_copy_file(source, spec.destination);
    if (err == CUP_OK) {
        err = filesystem_apply_required_permissions(spec.destination, spec.executable, spec.read_only);
        if (err != CUP_OK) err = CUP_ERR_COMMIT;
    }
    return err;
}

static CupError restore_old_asset(const char *old_dir, GenerationAssetId id) {
    GenerationAssetSpec spec;
    char old_path[MAX_PATH_LEN];
    SystemPathKind old_kind;
    SystemPathKind current_kind;
    CupError err = generation_asset_spec(id, &spec);
    if (err == CUP_OK) err = path_join(old_path, sizeof(old_path), old_dir, spec.release_name);
    if (err == CUP_OK) err = require_regular_or_missing(old_path, &old_kind);
    if (err == CUP_OK) err = require_regular_or_missing(spec.destination, &current_kind);
    if (err != CUP_OK) return err;
    if (old_kind == SYSTEM_PATH_MISSING) {
        if (current_kind == SYSTEM_PATH_MISSING) return CUP_OK;
        if (spec.read_only && system_set_read_only(spec.destination, 0) != CUP_OK) return CUP_ERR_TRANSACTION;
        return system_remove_file(spec.destination);
    }
    if (current_kind == SYSTEM_PATH_REGULAR_FILE && spec.read_only &&
        system_set_read_only(spec.destination, 0) != CUP_OK) return CUP_ERR_TRANSACTION;
    err = system_copy_file(old_path, spec.destination);
    if (err == CUP_OK) err = filesystem_apply_required_permissions(spec.destination, spec.executable, spec.read_only);
    return err;
}

static CupError finish_transaction(const UpdateJournal *journal,
                                   const char *staging,
                                   const char *message) {
    CupError err = runtime_journal_clear_if_identity(&journal->file_identity);
    if (err != CUP_OK) return err;
    if (filesystem_remove_tree(staging) != CUP_OK) {
        fprintf(stderr, "Warning: CUP generation transaction completed, but stale staging remains.\n");
    }
    if (message != NULL) printf("%s\n", message);
    return CUP_OK;
}

CupError update_generation_commit(const UpdateJournal *journal) {
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    char old_dir[MAX_PATH_LEN];
    int old_matches = 0;
    int target_matches = 0;
    CupError err;
    static const GenerationAssetId order[] = {
        CUP_GENERATION_ASSET_LICENSE,
        CUP_GENERATION_ASSET_NOTICES,
        CUP_GENERATION_ASSET_RELEASE,
        CUP_GENERATION_ASSET_BINARY};
    size_t i;

    if (journal == NULL || !journal->file_identity.valid) return CUP_ERR_INVALID_INPUT;
    err = update_journal_get_staging_path(journal, staging, sizeof(staging));
    if (err == CUP_OK) err = workspace_child(new_dir, sizeof(new_dir), staging, CUP_UPDATE_NEW_DIRECTORY);
    if (err == CUP_OK) err = workspace_child(old_dir, sizeof(old_dir), staging, CUP_UPDATE_OLD_DIRECTORY);
    if (err == CUP_OK) err = target_matches_installed(new_dir, journal->target_release_sha256, &target_matches);
    if (err == CUP_OK && target_matches) return finish_transaction(journal, staging, "Completed CUP generation transaction.");
    if (err == CUP_OK) err = canonical_matches_snapshot(old_dir, &old_matches);
    if (err != CUP_OK || !old_matches) return err != CUP_OK ? err : CUP_ERR_TRANSACTION;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        err = install_new_asset(new_dir, order[i]);
        if (err != CUP_OK) return err;
    }
    err = target_matches_installed(new_dir, journal->target_release_sha256, &target_matches);
    if (err != CUP_OK || !target_matches) return err != CUP_OK ? err : CUP_ERR_COMMIT;
    return finish_transaction(journal, staging, "Completed CUP generation transaction.");
}

CupError update_journal_recover(const UpdateJournal *journal, int *finalized) {
    char staging[MAX_PATH_LEN];
    char new_dir[MAX_PATH_LEN];
    char old_dir[MAX_PATH_LEN];
    int target_matches = 0;
    int binary_matches_old = 0;
    int old_matches = 0;
    CupError err;
    GenerationAssetId id;

    if (journal == NULL || !journal->file_identity.valid) return CUP_ERR_INVALID_INPUT;
    if (finalized != NULL) *finalized = 0;
    err = update_journal_get_staging_path(journal, staging, sizeof(staging));
    if (err == CUP_OK) err = workspace_child(new_dir, sizeof(new_dir), staging, CUP_UPDATE_NEW_DIRECTORY);
    if (err == CUP_OK) err = workspace_child(old_dir, sizeof(old_dir), staging, CUP_UPDATE_OLD_DIRECTORY);
    if (err == CUP_OK) err = target_matches_installed(new_dir, journal->target_release_sha256, &target_matches);
    if (err != CUP_OK) return err;
    if (target_matches) {
        err = finish_transaction(journal, staging, "Completed interrupted CUP generation transaction.");
        if (err == CUP_OK && finalized != NULL) *finalized = 1;
        return err;
    }
    err = canonical_binary_matches_old(old_dir, &binary_matches_old);
    if (err != CUP_OK || !binary_matches_old) return err != CUP_OK ? err : CUP_ERR_TRANSACTION;

    for (id = CUP_GENERATION_ASSET_RELEASE; id < CUP_GENERATION_ASSET_BINARY; ++id) {
        err = restore_old_asset(old_dir, id);
        if (err != CUP_OK) return err;
    }
    err = canonical_matches_snapshot(old_dir, &old_matches);
    if (err != CUP_OK || !old_matches) return err != CUP_OK ? err : CUP_ERR_TRANSACTION;
    return finish_transaction(journal, staging, "Rolled back interrupted CUP generation transaction.");
}
