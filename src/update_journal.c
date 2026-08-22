/*
 * Persists, validates and recovers the deferred self-update protocol. Package transactions remain
 * a separate journal owner.
 */

#include "update_journal.h"

#include "assets.h"
#include "checksum.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "release_metadata.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define CUP_UPDATE_JOURNAL_FORMAT "1"
#define FIELD_FORMAT (1u << 0)
#define FIELD_OPERATION (1u << 1)
#define FIELD_PHASE (1u << 2)
#define FIELD_TEMPORARY_NAME (1u << 3)
#define FIELD_TOKEN (1u << 4)
#define FIELD_VERSION (1u << 5)
#define FIELD_ERROR (1u << 6)
#define FIELD_RECOVERY (1u << 7)
#define JOURNAL_FIELDS \
    (FIELD_FORMAT | FIELD_OPERATION | FIELD_PHASE | FIELD_TEMPORARY_NAME | FIELD_TOKEN | \
     FIELD_VERSION | FIELD_ERROR | FIELD_RECOVERY)

typedef struct {
    const char *backup_name;
    const char *absent_name;
    const char *destination;
    int executable;
    int read_only;
} UpdateAsset;

/* Journal lifecycle and phase model. Scheduled, committing and failed states remain distinct across
 * process boundaries. */
void update_journal_init(UpdateJournal *journal) {
    if (journal != NULL) {
        memset(journal, 0, sizeof(*journal));
        journal->phase = CUP_UPDATE_PHASE_SCHEDULED;
        journal->recovery = CUP_UPDATE_FAILURE_NONE;
    }
}

const char *update_phase_name(UpdatePhase phase) {
    switch (phase) {
        case CUP_UPDATE_PHASE_SCHEDULED:
            return "scheduled";
        case CUP_UPDATE_PHASE_COMMITTING:
            return "committing";
        case CUP_UPDATE_PHASE_FAILED:
            return "failed";
        default:
            return "invalid";
    }
}

const char *update_failure_recovery_name(UpdateFailureRecovery recovery) {
    switch (recovery) {
        case CUP_UPDATE_FAILURE_NONE:
            return "none";
        case CUP_UPDATE_FAILURE_PENDING:
            return "pending";
        case CUP_UPDATE_FAILURE_ROLLED_BACK:
            return "rolled-back";
        default:
            return "invalid";
    }
}

static int parse_phase(const char *value, UpdatePhase *phase) {
    if (strcmp(value, "scheduled") == 0) {
        *phase = CUP_UPDATE_PHASE_SCHEDULED;
    } else if (strcmp(value, "committing") == 0) {
        *phase = CUP_UPDATE_PHASE_COMMITTING;
    } else if (strcmp(value, "failed") == 0) {
        *phase = CUP_UPDATE_PHASE_FAILED;
    } else {
        return 0;
    }
    return 1;
}

static int parse_failure_recovery(const char *value, UpdateFailureRecovery *recovery) {
    if (strcmp(value, "none") == 0) {
        *recovery = CUP_UPDATE_FAILURE_NONE;
    } else if (strcmp(value, "pending") == 0) {
        *recovery = CUP_UPDATE_FAILURE_PENDING;
    } else if (strcmp(value, "rolled-back") == 0) {
        *recovery = CUP_UPDATE_FAILURE_ROLLED_BACK;
    } else {
        return 0;
    }
    return 1;
}

static int temporary_name_is_valid(const char *name) {
    return path_is_safe_segment(name) && strlen(name) < MAX_METADATA_VALUE_LEN &&
           strncmp(name, "cup-update-", 11) == 0 && name[11] != '\0';
}

static int token_is_valid(const char *token) {
    size_t i;

    if (text_is_empty(token) || strlen(token) >= sizeof(((UpdateJournal *)0)->token)) {
        return 0;
    }
    /* Tokens embed the generated staging name; Windows temporary directories use a .tmp suffix. */
    for (i = 0; token[i] != '\0'; ++i) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static int token_matches_temporary_name(const char *token, const char *temporary_name) {
    size_t token_length;
    size_t name_length;

    if (!token_is_valid(token) || !temporary_name_is_valid(temporary_name)) {
        return 0;
    }
    token_length = strlen(token);
    name_length = strlen(temporary_name);
    return token_length > name_length && token[token_length - name_length - 1] == '-' &&
           strcmp(token + token_length - name_length, temporary_name) == 0;
}

static int error_code_is_valid(int error_code) {
    return error_code >= (int)CUP_ERR_INVALID_INPUT && error_code <= (int)CUP_ERR_INTERRUPT;
}

static int journal_fields_are_coherent(const UpdateJournal *journal) {
    if (journal == NULL || !temporary_name_is_valid(journal->temporary_name) ||
        !token_matches_temporary_name(journal->token, journal->temporary_name) ||
        release_version_parse(journal->version, NULL) != CUP_OK ||
        strcmp(update_phase_name(journal->phase), "invalid") == 0 ||
        strcmp(update_failure_recovery_name(journal->recovery), "invalid") == 0) {
        return 0;
    }
    if (journal->phase == CUP_UPDATE_PHASE_FAILED) {
        return error_code_is_valid(journal->error_code) &&
               (journal->recovery == CUP_UPDATE_FAILURE_PENDING ||
                journal->recovery == CUP_UPDATE_FAILURE_ROLLED_BACK);
    }
    return journal->error_code == 0 && journal->recovery == CUP_UPDATE_FAILURE_NONE;
}

static CupError write_journal(FILE *file, const void *value) {
    const UpdateJournal *journal = value;

    if (journal == NULL ||
        fprintf(file, "format=%s\n", CUP_UPDATE_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=cup-update\n") < 0 ||
        fprintf(file, "phase=%s\n", update_phase_name(journal->phase)) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0 ||
        fprintf(file, "token=%s\n", journal->token) < 0 ||
        fprintf(file, "version=%s\n", journal->version) < 0 ||
        fprintf(file, "error=%d\n", journal->error_code) < 0 ||
        fprintf(file,
                "recovery=%s\n",
                update_failure_recovery_name(journal->recovery)) < 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError save_journal(UpdateJournal *journal) {
    char staging_dir[MAX_PATH_LEN];
    SystemPathIdentity published_identity;
    const SystemPathIdentity *expected_identity;
    CupError err;

    if (!journal_fields_are_coherent(journal) ||
        layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    expected_identity = journal->file_identity.valid ? &journal->file_identity : NULL;
    err = runtime_journal_publish(staging_dir,
                                  "transaction",
                                  expected_identity,
                                  write_journal,
                                  journal,
                                  &published_identity);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && published_identity.valid) {
        journal->file_identity = published_identity;
    }
    return err;
}

CupError update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version,
                                  UpdateJournal *created) {
    UpdateJournal journal;
    CupError err;
    const char *name;

    if (created == NULL || text_is_empty(temporary_path) || !token_is_valid(token) ||
        release_version_parse(version, NULL) != CUP_OK || strlen(version) >= MAX_IDENTIFIER_LEN) {
        return CUP_ERR_INVALID_INPUT;
    }
    update_journal_init(created);
    name = path_last_segment(temporary_path);
    if (!temporary_name_is_valid(name) || !token_matches_temporary_name(token, name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    update_journal_init(&journal);
    if (text_copy(journal.temporary_name, sizeof(journal.temporary_name), name) != CUP_OK ||
        text_copy(journal.token, sizeof(journal.token), token) != CUP_OK ||
        text_copy(journal.version, sizeof(journal.version), version) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    err = save_journal(&journal);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && journal.file_identity.valid) {
        *created = journal;
    }
    return err;
}


CupError update_journal_set_phase(UpdateJournal *journal,
                                      UpdatePhase phase,
                                      int error_code) {
    UpdateJournal candidate;
    CupError err;

    if (journal == NULL || strcmp(update_phase_name(phase), "invalid") == 0 ||
        (phase == CUP_UPDATE_PHASE_FAILED ? !error_code_is_valid(error_code)
                                          : error_code != 0)) {
        return CUP_ERR_INVALID_INPUT;
    }

    candidate = *journal;
    candidate.phase = phase;
    candidate.error_code = error_code;
    candidate.recovery = phase == CUP_UPDATE_PHASE_FAILED ? CUP_UPDATE_FAILURE_PENDING
                                                           : CUP_UPDATE_FAILURE_NONE;
    err = save_journal(&candidate);
    if (err == CUP_OK || err == CUP_ERR_COMMIT) {
        *journal = candidate;
    }
    return err;
}

static CupError set_failure_recovery(UpdateJournal *journal,
                                     UpdateFailureRecovery recovery) {
    UpdateJournal candidate;
    CupError err;

    if (journal == NULL || journal->phase != CUP_UPDATE_PHASE_FAILED ||
        recovery == CUP_UPDATE_FAILURE_NONE ||
        strcmp(update_failure_recovery_name(recovery), "invalid") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    candidate = *journal;
    candidate.recovery = recovery;
    err = save_journal(&candidate);
    if (err == CUP_OK || err == CUP_ERR_COMMIT) {
        *journal = candidate;
    }
    return err;
}

/* Strict journal decoding. Unknown, duplicate or inconsistent fields preserve the journal as a
 * blocker. */
static CupError set_field(UpdateJournal *journal,
                                     const char *key,
                                     const char *value,
                                     unsigned *seen) {
    unsigned bit;
    CupError err = CUP_OK;

    if (strcmp(key, "format") == 0) {
        bit = FIELD_FORMAT;
        if (strcmp(value, CUP_UPDATE_JOURNAL_FORMAT) != 0) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        bit = FIELD_OPERATION;
        if (strcmp(value, "cup-update") != 0) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "phase") == 0) {
        bit = FIELD_PHASE;
        if (!parse_phase(value, &journal->phase)) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "temporary_name") == 0) {
        bit = FIELD_TEMPORARY_NAME;
        if (text_copy(journal->temporary_name, sizeof(journal->temporary_name), value) != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "token") == 0) {
        bit = FIELD_TOKEN;
        if (text_copy(journal->token, sizeof(journal->token), value) != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "version") == 0) {
        bit = FIELD_VERSION;
        if (text_copy(journal->version, sizeof(journal->version), value) != CUP_OK ||
            text_is_empty(value)) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "error") == 0) {
        unsigned parsed;

        bit = FIELD_ERROR;
        if (!text_parse_uint(value, 255u, &parsed)) {
            err = CUP_ERR_TRANSACTION;
        } else {
            journal->error_code = (int)parsed;
        }
    } else if (strcmp(key, "recovery") == 0) {
        bit = FIELD_RECOVERY;
        if (!parse_failure_recovery(value, &journal->recovery)) {
            err = CUP_ERR_TRANSACTION;
        }
    } else {
        return CUP_ERR_TRANSACTION;
    }

    if (err != CUP_OK || (*seen & bit) != 0) {
        return CUP_ERR_TRANSACTION;
    }
    *seen |= bit;
    return CUP_OK;
}

static const char *const journal_keys[] = {
    "format", "operation", "phase", "temporary_name",
    "token", "version", "error", "recovery"};

typedef struct {
    UpdateJournal *candidate;
    unsigned seen;
} UpdateJournalParser;

static CupError parse_field(const char *key,
                                               const char *value,
                                               void *userdata) {
    UpdateJournalParser *parser = userdata;

    if (parser == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    return set_field(parser->candidate, key, value, &parser->seen);
}

CupError update_journal_load(UpdateJournal *journal, UpdateJournalStatus *status) {
    UpdateJournal candidate;
    UpdateJournalParser parser;
    SystemPathIdentity file_identity;
    CupError err;
    int missing;

    if (journal == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    update_journal_init(journal);
    update_journal_init(&candidate);
    memset(&parser, 0, sizeof(parser));
    memset(&file_identity, 0, sizeof(file_identity));
    parser.candidate = &candidate;
    *status = CUP_UPDATE_JOURNAL_MISSING;

    err = runtime_journal_parse(journal_keys,
                                sizeof(journal_keys) /
                                    sizeof(journal_keys[0]),
                                parse_field,
                                &parser,
                                &file_identity,
                                &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (parser.seen != JOURNAL_FIELDS ||
        !journal_fields_are_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.file_identity = file_identity;

    *journal = candidate;
    *status = CUP_UPDATE_JOURNAL_LOADED;
    return CUP_OK;
}

CupError update_journal_get_staging_path(const UpdateJournal *journal,
                                         char *buffer,
                                         size_t size) {
    char staging_dir[MAX_PATH_LEN];

    if (journal == NULL || buffer == NULL || size == 0 ||
        !temporary_name_is_valid(journal->temporary_name)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return path_join(buffer, size, staging_dir, journal->temporary_name);
}

#define CUP_UPDATE_GENERATION_FORMAT "1"
#define CUP_UPDATE_GENERATION_LINES 7u

typedef struct {
    char version[MAX_IDENTIFIER_LEN];
    char binary[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char platform_checksums[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char packages[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char install_policy[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char common_checksums[CHECKSUM_SHA256_HEX_LENGTH + 1];
    SystemPathIdentity file_identity;
} UpdateGeneration;

static CupError resolve_generation_paths(char paths[5][MAX_PATH_LEN]) {
    CupError err = layout_get_binary_path(paths[0], MAX_PATH_LEN);

    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(paths[1], MAX_PATH_LEN);
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(paths[2], MAX_PATH_LEN);
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(paths[3], MAX_PATH_LEN);
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(paths[4], MAX_PATH_LEN);
    }
    return err;
}

static CupError hash_generation_paths(const char *binary,
                                      char paths[5][MAX_PATH_LEN],
                                      UpdateGeneration *generation) {
    CupError err;

    if (text_is_empty(binary) || generation == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = checksum_sha256_file(binary, generation->binary, sizeof(generation->binary));
    if (err == CUP_OK) {
        err = checksum_sha256_file(paths[1], generation->platform_checksums,
                                   sizeof(generation->platform_checksums));
    }
    if (err == CUP_OK) {
        err = checksum_sha256_file(paths[2], generation->packages, sizeof(generation->packages));
    }
    if (err == CUP_OK) {
        err = checksum_sha256_file(paths[3], generation->install_policy,
                                   sizeof(generation->install_policy));
    }
    if (err == CUP_OK) {
        err = checksum_sha256_file(paths[4], generation->common_checksums,
                                   sizeof(generation->common_checksums));
    }
    return err;
}

CupError update_write_generation_marker(const char *staging,
                                            const char *version,
                                            const char *staged_binary) {
    UpdateGeneration generation;
    char paths[5][MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;
    int failed = 0;

    if (text_is_empty(staging) || release_version_parse(version, NULL) != CUP_OK ||
        text_is_empty(staged_binary)) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(&generation, 0, sizeof(generation));
    err = text_copy(generation.version, sizeof(generation.version), version);
    if (err == CUP_OK) {
        err = resolve_generation_paths(paths);
    }
    if (err == CUP_OK) {
        err = hash_generation_paths(staged_binary, paths, &generation);
    }
    if (err == CUP_OK) {
        err = path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED);
    }
    if (err == CUP_OK) {
        err = system_create_file_exclusive(marker, &file);
    }
    if (err != CUP_OK) {
        return err;
    }

    if (fprintf(file, "format=%s\n", CUP_UPDATE_GENERATION_FORMAT) < 0 ||
        fprintf(file, "version=%s\n", generation.version) < 0 ||
        fprintf(file, "binary_sha256=%s\n", generation.binary) < 0 ||
        fprintf(file, "platform_checksums_sha256=%s\n", generation.platform_checksums) < 0 ||
        fprintf(file, "packages_sha256=%s\n", generation.packages) < 0 ||
        fprintf(file, "install_policy_sha256=%s\n", generation.install_policy) < 0 ||
        fprintf(file, "common_checksums_sha256=%s\n", generation.common_checksums) < 0 ||
        system_sync_file(file) != CUP_OK) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed || system_sync_parent_directory(marker) != CUP_OK) {
        /* The marker may already be visible. Preserve it as recovery evidence rather than
         * deleting a pathname that could have been replaced after creation. */
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

static CupError load_generation_marker(const char *path, UpdateGeneration *generation) {
    static const char *const keys[CUP_UPDATE_GENERATION_LINES] = {
        "format", "version", "binary_sha256", "platform_checksums_sha256",
        "packages_sha256", "install_policy_sha256", "common_checksums_sha256"};
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    char line[256];
    CupError err;
    int missing;
    size_t index = 0;

    if (text_is_empty(path) || generation == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(generation, 0, sizeof(*generation));
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(path, 4096u, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_TRANSACTION;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    while (err == CUP_OK) {
        char key[64];
        char value[128];
        int has_line;

        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK || !has_line) {
            break;
        }
        if (index >= CUP_UPDATE_GENERATION_LINES ||
            text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK ||
            strcmp(key, keys[index]) != 0) {
            err = CUP_ERR_TRANSACTION;
            break;
        }
        if (index == 0) {
            if (strcmp(value, CUP_UPDATE_GENERATION_FORMAT) != 0) {
                err = CUP_ERR_TRANSACTION;
            }
        } else if (index == 1) {
            if (release_version_parse(value, NULL) != CUP_OK ||
                text_copy(generation->version, sizeof(generation->version), value) != CUP_OK) {
                err = CUP_ERR_TRANSACTION;
            }
        } else {
            char *destination[] = {generation->binary, generation->platform_checksums,
                                   generation->packages, generation->install_policy,
                                   generation->common_checksums};
            if (!checksum_digest_is_canonical(value) ||
                text_copy(destination[index - 2],
                          CHECKSUM_SHA256_HEX_LENGTH + 1,
                          value) != CUP_OK) {
                err = CUP_ERR_TRANSACTION;
            }
        }
        index++;
    }
    if (err == CUP_OK && index != CUP_UPDATE_GENERATION_LINES) {
        err = CUP_ERR_TRANSACTION;
    }
    if (err == CUP_OK) {
        generation->file_identity = snapshot.identity;
    }
    filesystem_snapshot_release(&snapshot);
    return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError installed_generation_matches(const UpdateGeneration *expected, int *matches) {
    UpdateGeneration current;
    char paths[5][MAX_PATH_LEN];
    CupError err;

    if (expected == NULL || matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;
    memset(&current, 0, sizeof(current));
    err = resolve_generation_paths(paths);
    if (err == CUP_OK) {
        err = hash_generation_paths(paths[0], paths, &current);
    }
    if (err == CUP_OK) {
        *matches = strcmp(current.binary, expected->binary) == 0 &&
                   strcmp(current.platform_checksums, expected->platform_checksums) == 0 &&
                   strcmp(current.packages, expected->packages) == 0 &&
                   strcmp(current.install_policy, expected->install_policy) == 0 &&
                   strcmp(current.common_checksums, expected->common_checksums) == 0;
    }
    return err;
}


/* cup update recovery. */
static CupError files_are_equal(const char *left, const char *right, int *equal) {
    char left_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    char right_hash[CHECKSUM_SHA256_HEX_LENGTH + 1];
    CupError err;

    if (text_is_empty(left) || text_is_empty(right) || equal == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *equal = 0;
    err = checksum_sha256_file(left, left_hash, sizeof(left_hash));
    if (err == CUP_OK) {
        err = checksum_sha256_file(right, right_hash, sizeof(right_hash));
    }
    if (err == CUP_OK) {
        *equal = strcmp(left_hash, right_hash) == 0;
    }
    return err;
}

static CupError asset_evidence_kinds(const char *staging,
                                     const UpdateAsset *asset,
                                     char *backup,
                                     size_t backup_size,
                                     SystemPathKind *backup_kind,
                                     char *absent,
                                     size_t absent_size,
                                     SystemPathKind *absent_kind) {
    CupError err = path_join(backup, backup_size, staging, asset->backup_name);

    if (err == CUP_OK) {
        err = path_join(absent, absent_size, staging, asset->absent_name);
    }
    if (err == CUP_OK) {
        err = system_get_path_kind(backup, backup_kind);
    }
    if (err == CUP_OK) {
        err = system_get_path_kind(absent, absent_kind);
    }
    if (err != CUP_OK ||
        (*backup_kind == SYSTEM_PATH_REGULAR_FILE) ==
            (*absent_kind == SYSTEM_PATH_REGULAR_FILE) ||
        (*backup_kind != SYSTEM_PATH_MISSING &&
         *backup_kind != SYSTEM_PATH_REGULAR_FILE) ||
        (*absent_kind != SYSTEM_PATH_MISSING &&
         *absent_kind != SYSTEM_PATH_REGULAR_FILE)) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError validate_preserved_binary(const char *staging, const UpdateAsset *asset) {
    char backup[MAX_PATH_LEN];
    char absent[MAX_PATH_LEN];
    SystemPathKind backup_kind;
    SystemPathKind absent_kind;
    SystemPathKind destination_kind;
    CupError err;
    int equal;

    err = asset_evidence_kinds(staging,
                               asset,
                               backup,
                               sizeof(backup),
                               &backup_kind,
                               absent,
                               sizeof(absent),
                               &absent_kind);
    if (err == CUP_OK) {
        err = system_get_path_kind(asset->destination, &destination_kind);
    }
    if (err != CUP_OK || destination_kind != SYSTEM_PATH_REGULAR_FILE ||
        absent_kind == SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }
    err = files_are_equal(backup, asset->destination, &equal);
    return err == CUP_OK && equal ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError remove_bootstrap_destination(const UpdateAsset *asset,
                                             SystemPathKind destination_kind) {
    SystemPathIdentity identity;
    CupError err;

    if (destination_kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (destination_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }
    err = system_get_path_identity(asset->destination, &identity);
    if (err == CUP_OK && asset->read_only) {
        err = system_set_read_only(asset->destination, 0);
    }
    if (err == CUP_OK) {
        err = system_remove_file_if_identity(asset->destination, &identity);
    }
    if (err == CUP_OK) {
        err = system_sync_parent_directory(asset->destination);
    }
    return err == CUP_OK ? CUP_OK : CUP_ERR_ROLLBACK;
}

/* Recovery after helper interruption. The committed marker decides whether backups are restored or
 * cleanup is completed. */
static CupError restore_asset(const char *staging,
                                         const UpdateAsset *asset,
                                         int preserve_destination) {
    char backup[MAX_PATH_LEN];
    char absent[MAX_PATH_LEN];
    CupError err;
    SystemPathKind backup_kind;
    SystemPathKind absent_kind;
    SystemPathKind destination_kind = SYSTEM_PATH_MISSING;

    err = asset_evidence_kinds(staging,
                               asset,
                               backup,
                               sizeof(backup),
                               &backup_kind,
                               absent,
                               sizeof(absent),
                               &absent_kind);
    if (err == CUP_OK) {
        err = system_get_path_kind(asset->destination, &destination_kind);
    }
    if (err != CUP_OK ||
        (destination_kind != SYSTEM_PATH_MISSING &&
         destination_kind != SYSTEM_PATH_REGULAR_FILE)) {
        return CUP_ERR_TRANSACTION;
    }
    if (absent_kind == SYSTEM_PATH_REGULAR_FILE) {
        return preserve_destination ? CUP_ERR_TRANSACTION
                                    : remove_bootstrap_destination(asset, destination_kind);
    }
    if (preserve_destination) {
        int equal;

        if (destination_kind != SYSTEM_PATH_REGULAR_FILE ||
            files_are_equal(backup, asset->destination, &equal) != CUP_OK || !equal) {
            return CUP_ERR_TRANSACTION;
        }
        err = filesystem_apply_required_permissions(
            asset->destination, asset->executable, asset->read_only);
        return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
    }
    if (destination_kind == SYSTEM_PATH_REGULAR_FILE &&
        system_set_read_only(asset->destination, 0) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    /* Keep the sole rollback backup intact until the complete recovery succeeds. The copy
     * primitive publishes through its own sibling temporary, so an interrupted repair can retry
     * from the same .old evidence instead of consuming it during the first restore attempt. */
    err = system_copy_file(backup, asset->destination);
    if (err != CUP_OK) {
        return err == CUP_ERR_COMMIT ? CUP_ERR_COMMIT : CUP_ERR_ROLLBACK;
    }
    err = filesystem_apply_required_permissions(
        asset->destination, asset->executable, asset->read_only);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: could not restore permissions for update asset '%s'.\n",
                asset->destination);
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

static CupError all_assets_were_absent(const char *staging,
                                       const UpdateAsset *assets,
                                       size_t count,
                                       int *all_absent) {
    size_t i;

    *all_absent = 1;
    for (i = 0; i < count; ++i) {
        char path[MAX_PATH_LEN];
        SystemPathKind kind;
        CupError err = path_join(path, sizeof(path), staging, assets[i].absent_name);

        if (err == CUP_OK) {
            err = system_get_path_kind(path, &kind);
        }
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (kind == SYSTEM_PATH_MISSING) {
            *all_absent = 0;
        } else if (kind != SYSTEM_PATH_REGULAR_FILE) {
            return CUP_ERR_TRANSACTION;
        }
    }
    return CUP_OK;
}

typedef struct {
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char catalog[MAX_PATH_LEN];
    char install_policy[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    SystemPathKind marker_kind;
    UpdateAsset assets[5];
} UpdateRecoveryPlan;

static CupError acknowledge_rollback(const UpdateJournal *journal,
                                               UpdateRecoveryResult *result) {
    AssetsInspection inspection;
    char staging[MAX_PATH_LEN];
    SystemPathKind staging_kind;
    CupError err;

    err = update_journal_get_staging_path(journal, staging, sizeof(staging));
    if (err == CUP_OK) {
        err = system_get_path_kind(staging, &staging_kind);
    }
    if (err != CUP_OK ||
        (staging_kind != SYSTEM_PATH_MISSING &&
         staging_kind != SYSTEM_PATH_DIRECTORY)) {
        return CUP_ERR_TRANSACTION;
    }

    /* Deleting the final recovery evidence is safe only after the installed generation
     * has been validated independently of the journal. */
    err = assets_inspect(&inspection);
    if (err != CUP_OK || !assets_installed_is_valid(&inspection)) {
        return CUP_ERR_TRANSACTION;
    }

    if (staging_kind == SYSTEM_PATH_DIRECTORY) {
        err = filesystem_remove_tree(staging);
        if (err == CUP_OK) {
            err = system_get_path_kind(staging, &staging_kind);
        }
        if (err != CUP_OK || staging_kind != SYSTEM_PATH_MISSING) {
            return CUP_ERR_TRANSACTION;
        }
    }

    err = runtime_journal_clear_if_identity(&journal->file_identity);
    if (err != CUP_OK) {
        return err;
    }

    if (result != NULL) {
        *result = CUP_UPDATE_RECOVERY_ACKNOWLEDGED;
    }
    printf("Acknowledged failed cup update to version %s after a successful rollback "
           "(error %d).\n",
           journal->version,
           journal->error_code);
    return CUP_OK;
}

static CupError resolve_recovery_plan(const UpdateJournal *journal,
                                      UpdateRecoveryPlan *plan) {
    CupError err;

    memset(plan, 0, sizeof(*plan));
    plan->marker_kind = SYSTEM_PATH_MISSING;

    err = update_journal_get_staging_path(
        journal, plan->staging, sizeof(plan->staging));
    if (err == CUP_OK) {
        err = path_join(
            plan->marker, sizeof(plan->marker), plan->staging, CUP_UPDATE_COMMITTED);
    }
    if (err == CUP_OK) {
        err = layout_get_binary_path(plan->binary, sizeof(plan->binary));
    }
    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(
            plan->platform_checksums, sizeof(plan->platform_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(plan->catalog, sizeof(plan->catalog));
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(
            plan->install_policy, sizeof(plan->install_policy));
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(
            plan->common_checksums, sizeof(plan->common_checksums));
    }
    if (err == CUP_OK) {
        err = system_get_path_kind(plan->marker, &plan->marker_kind);
    }
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    plan->assets[0] = (UpdateAsset){
        CUP_UPDATE_BINARY_OLD, CUP_UPDATE_BINARY_ABSENT, plan->binary, 1, 0};
    plan->assets[1] = (UpdateAsset){CUP_UPDATE_PLATFORM_CHECKSUMS_OLD,
                                      CUP_UPDATE_PLATFORM_CHECKSUMS_ABSENT,
                                      plan->platform_checksums,
                                      0,
                                      1};
    plan->assets[2] = (UpdateAsset){
        CUP_UPDATE_PACKAGES_OLD, CUP_UPDATE_PACKAGES_ABSENT, plan->catalog, 0, 1};
    plan->assets[3] = (UpdateAsset){CUP_UPDATE_INSTALL_POLICY_OLD,
                                      CUP_UPDATE_INSTALL_POLICY_ABSENT,
                                      plan->install_policy,
                                      0,
                                      1};
    plan->assets[4] = (UpdateAsset){CUP_UPDATE_COMMON_CHECKSUMS_OLD,
                                      CUP_UPDATE_COMMON_CHECKSUMS_ABSENT,
                                      plan->common_checksums,
                                      0,
                                      1};
    return CUP_OK;
}

static CupError recover_committed_generation(const UpdateJournal *journal,
                                             UpdateRecoveryPlan *plan,
                                             UpdateRecoveryResult *result,
                                             int *finalized) {
    AssetsInspection inspection;
    UpdateGeneration generation;
    CupError err;
    int generation_matches = 0;

    *finalized = 0;
    if (plan->marker_kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (plan->marker_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }

    err = load_generation_marker(plan->marker, &generation);
    if (err != CUP_OK || strcmp(generation.version, journal->version) != 0) {
        return CUP_ERR_TRANSACTION;
    }

    err = installed_generation_matches(&generation, &generation_matches);
    if (err != CUP_OK) {
        /* An unreadable installed generation cannot grant rollback authority. */
        return CUP_ERR_TRANSACTION;
    }

    if (generation_matches) {
        err = assets_inspect(&inspection);
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (assets_installed_is_valid(&inspection)) {
            err = runtime_journal_clear_if_identity(&journal->file_identity);
            if (err != CUP_OK) {
                return err;
            }
            if (filesystem_remove_tree(plan->staging) != CUP_OK) {
                fprintf(stderr,
                        "Warning: the cup update completed, but stale update staging could "
                        "not be removed. Run 'cup repair'.\n");
            }
            if (result != NULL) {
                *result = CUP_UPDATE_RECOVERY_FINALIZED;
            }
            *finalized = 1;
            printf("Completed interrupted cup update transaction.\n");
            return CUP_OK;
        }
    }

    /* Remove the exact stale commit decision before restoring any backup. */
    err = system_remove_file_if_identity(plan->marker, &generation.file_identity);
    if (err != CUP_OK || system_sync_parent_directory(plan->marker) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    plan->marker_kind = SYSTEM_PATH_MISSING;
    return CUP_OK;
}

static CupError restore_recovery_assets(const UpdateRecoveryPlan *plan,
                                        UpdateRecoveryMode mode) {
    CupError err;
    size_t i;

    if (mode == CUP_UPDATE_RECOVER_PRESERVE_BINARY &&
        validate_preserved_binary(plan->staging, &plan->assets[0]) != CUP_OK) {
        fprintf(stderr,
                "Error: interrupted cup update recovery would replace the running executable. "
                "Repair preserved cup and all transaction evidence; run the official installer "
                "to recover safely.\n");
        return CUP_ERR_TRANSACTION;
    }

    /* Supporting assets are restored before the executable. */
    for (i = 1; i < sizeof(plan->assets) / sizeof(plan->assets[0]); ++i) {
        err = restore_asset(plan->staging, &plan->assets[i], 0);
        if (err != CUP_OK) {
            return err;
        }
    }
    return restore_asset(
        plan->staging,
        &plan->assets[0],
        mode == CUP_UPDATE_RECOVER_PRESERVE_BINARY);
}

static CupError finish_recovery_rollback(const UpdateJournal *journal,
                                         const UpdateRecoveryPlan *plan,
                                         UpdateRecoveryResult *result) {
    AssetsInspection inspection;
    CupError err;
    int bootstrap_rollback = 0;

    err = all_assets_were_absent(plan->staging,
                                 plan->assets,
                                 sizeof(plan->assets) / sizeof(plan->assets[0]),
                                 &bootstrap_rollback);
    if (err != CUP_OK) {
        return err;
    }

    if (bootstrap_rollback) {
        err = runtime_journal_clear_if_identity(&journal->file_identity);
        if (err != CUP_OK) {
            return err;
        }
        if (filesystem_remove_tree(plan->staging) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (result != NULL) {
            *result = CUP_UPDATE_RECOVERY_ROLLED_BACK;
        }
        printf("Rolled back failed initial cup installation.\n");
        return CUP_OK;
    }

    err = assets_inspect(&inspection);
    if (err != CUP_OK || !assets_installed_is_valid(&inspection)) {
        return CUP_ERR_TRANSACTION;
    }

    if (journal->phase == CUP_UPDATE_PHASE_FAILED) {
        UpdateJournal recovered = *journal;

        err = set_failure_recovery(&recovered, CUP_UPDATE_FAILURE_ROLLED_BACK);
        if (err != CUP_OK) {
            return err;
        }
        if (filesystem_remove_tree(plan->staging) != CUP_OK) {
            fprintf(stderr,
                    "Warning: the failed cup update was rolled back, but stale staging could "
                    "not be removed. Run 'cup repair' again.\n");
        }
        printf("Rolled back failed cup update to version %s. Run 'cup repair' again to "
               "acknowledge the recorded failure.\n",
               journal->version);
    } else {
        err = runtime_journal_clear_if_identity(&journal->file_identity);
        if (err != CUP_OK) {
            return err;
        }
        if (filesystem_remove_tree(plan->staging) != CUP_OK) {
            fprintf(stderr,
                    "Warning: the cup update was rolled back, but stale update staging could "
                    "not be removed. Run 'cup repair'.\n");
        }
        printf("Rolled back interrupted cup update transaction.\n");
    }

    if (result != NULL) {
        *result = CUP_UPDATE_RECOVERY_ROLLED_BACK;
    }
    return CUP_OK;
}

static CupError recover_scheduled(const UpdateJournal *journal,
                                         UpdateRecoveryResult *result) {
    char staging[MAX_PATH_LEN];
    CupError err;

    err = update_journal_get_staging_path(journal, staging, sizeof(staging));
    if (err != CUP_OK) {
        return err;
    }

    /* scheduled is the durable promise that no canonical CUP asset has been changed yet. Any
     * staging evidence is therefore reconstructible and may be discarded without rollback. */
    err = runtime_journal_clear_if_identity(&journal->file_identity);
    if (err != CUP_OK) {
        return err;
    }
    if (filesystem_remove_tree(staging) != CUP_OK) {
        fprintf(stderr,
                "Warning: the pending cup update was cancelled, but stale update staging could "
                "not be removed. Run 'cup repair'.\n");
    }
    if (result != NULL) {
        *result = CUP_UPDATE_RECOVERY_ROLLED_BACK;
    }
    printf("Cancelled pending cup update before any installed asset was changed.\n");
    return CUP_OK;
}

CupError update_journal_recover(const UpdateJournal *journal,
                                    UpdateRecoveryMode mode,
                                    UpdateRecoveryResult *result) {
    UpdateRecoveryPlan plan;
    CupError err;
    int finalized;

    if (journal == NULL ||
        (mode != CUP_UPDATE_RECOVER_REPLACE_BINARY &&
         mode != CUP_UPDATE_RECOVER_PRESERVE_BINARY)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (result != NULL) {
        *result = CUP_UPDATE_RECOVERY_NONE;
    }

    if (journal->phase == CUP_UPDATE_PHASE_FAILED &&
        journal->recovery == CUP_UPDATE_FAILURE_ROLLED_BACK) {
        return acknowledge_rollback(journal, result);
    }
    if (journal->phase == CUP_UPDATE_PHASE_SCHEDULED) {
        return recover_scheduled(journal, result);
    }

    err = resolve_recovery_plan(journal, &plan);
    if (err != CUP_OK) {
        return err;
    }

    err = recover_committed_generation(journal, &plan, result, &finalized);
    if (err != CUP_OK || finalized) {
        return err;
    }

    err = restore_recovery_assets(&plan, mode);
    if (err != CUP_OK) {
        return err;
    }

    return finish_recovery_rollback(journal, &plan, result);
}
