/*
 * Persists, validates and recovers the deferred cup update protocol. Package transactions remain
 * a separate journal owner.
 */

#include "cup_update_journal.h"

#include "cup_assets.h"
#include "checksum.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUP_UPDATE_JOURNAL_FORMAT "1"
#define CUP_UPDATE_JOURNAL_LINE_LEN 512
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
    const char *destination;
    int executable;
    int read_only;
} CupUpdateAsset;

/* Journal lifecycle and phase model. Scheduled, committing and failed states remain distinct across
 * process boundaries. */
void cup_update_journal_init(CupUpdateJournal *journal) {
    if (journal != NULL) {
        memset(journal, 0, sizeof(*journal));
        journal->phase = CUP_UPDATE_PHASE_SCHEDULED;
        journal->recovery = CUP_UPDATE_FAILURE_NONE;
    }
}

const char *cup_update_phase_name(CupUpdatePhase phase) {
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

const char *cup_update_failure_recovery_name(CupUpdateFailureRecovery recovery) {
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

static int parse_phase(const char *value, CupUpdatePhase *phase) {
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

static int parse_failure_recovery(const char *value, CupUpdateFailureRecovery *recovery) {
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

static int concrete_version_is_valid(const char *value) {
    const char *cursor = value;
    size_t part;

    if (text_is_empty(value)) {
        return 0;
    }
    for (part = 0; part < 3; ++part) {
        unsigned long number = 0;
        size_t digits = 0;

        if (*cursor < '0' || *cursor > '9' ||
            (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')) {
            return 0;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            number = number * 10u + (unsigned long)(*cursor - '0');
            if (number > 999999u) {
                return 0;
            }
            cursor++;
            digits++;
        }
        if (digits == 0) {
            return 0;
        }
        if (part < 2) {
            if (*cursor != '.') {
                return 0;
            }
            cursor++;
        } else if (*cursor != '\0') {
            return 0;
        }
    }
    return 1;
}

static int temporary_name_is_valid(const char *name) {
    return path_is_safe_segment(name) && strncmp(name, "cup-update-", 11) == 0 && name[11] != '\0';
}

static int token_is_valid(const char *token) {
    size_t i;

    if (text_is_empty(token) || strlen(token) >= MAX_PATH_LEN) {
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

static int journal_fields_are_coherent(const CupUpdateJournal *journal) {
    if (journal == NULL || !temporary_name_is_valid(journal->temporary_name) ||
        !token_matches_temporary_name(journal->token, journal->temporary_name) ||
        !concrete_version_is_valid(journal->version) ||
        strcmp(cup_update_phase_name(journal->phase), "invalid") == 0 ||
        strcmp(cup_update_failure_recovery_name(journal->recovery), "invalid") == 0) {
        return 0;
    }
    if (journal->phase == CUP_UPDATE_PHASE_FAILED) {
        return journal->error_code > 0 &&
               (journal->recovery == CUP_UPDATE_FAILURE_PENDING ||
                journal->recovery == CUP_UPDATE_FAILURE_ROLLED_BACK);
    }
    return journal->error_code == 0 && journal->recovery == CUP_UPDATE_FAILURE_NONE;
}

static CupError save_cup_update_journal(const CupUpdateJournal *journal) {
    CupError err;
    FILE *file = NULL;
    char path[MAX_PATH_LEN];
    char staging_dir[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int failed = 0;

    if (!journal_fields_are_coherent(journal) ||
        layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK ||
        system_create_temp_file(staging_dir, "transaction", temporary, sizeof(temporary), &file) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (fprintf(file, "format=%s\n", CUP_UPDATE_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=cup-update\n") < 0 ||
        fprintf(file, "phase=%s\n", cup_update_phase_name(journal->phase)) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0 ||
        fprintf(file, "token=%s\n", journal->token) < 0 ||
        fprintf(file, "version=%s\n", journal->version) < 0 ||
        fprintf(file, "error=%d\n", journal->error_code) < 0 ||
        fprintf(file, "recovery=%s\n",
                cup_update_failure_recovery_name(journal->recovery)) < 0 ||
        system_sync_file(file) != CUP_OK) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }

    err = system_replace_file(temporary, path, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }
    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }
    return CUP_ERR_COMMIT;
}

CupError cup_update_journal_begin(const char *temporary_path,
                                  const char *token,
                                  const char *version) {
    CupUpdateJournal journal;
    const char *name;
    char path[MAX_PATH_LEN];
    int exists;

    if (text_is_empty(temporary_path) || !token_is_valid(token) ||
        !concrete_version_is_valid(version) || strlen(version) >= MAX_IDENTIFIER_LEN) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (exists) {
        fprintf(stderr, "Error: an interrupted cup operation must be repaired first.\n");
        return CUP_ERR_TRANSACTION;
    }

    name = path_last_segment(temporary_path);
    if (!temporary_name_is_valid(name) || !token_matches_temporary_name(token, name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    cup_update_journal_init(&journal);
    if (text_copy(journal.temporary_name, sizeof(journal.temporary_name), name) != CUP_OK ||
        text_copy(journal.token, sizeof(journal.token), token) != CUP_OK ||
        text_copy(journal.version, sizeof(journal.version), version) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return save_cup_update_journal(&journal);
}

CupError cup_update_journal_set_phase(CupUpdateJournal *journal,
                                      CupUpdatePhase phase,
                                      int error_code) {
    if (journal == NULL || strcmp(cup_update_phase_name(phase), "invalid") == 0 ||
        (phase == CUP_UPDATE_PHASE_FAILED ? error_code <= 0 : error_code != 0)) {
        return CUP_ERR_INVALID_INPUT;
    }
    journal->phase = phase;
    journal->error_code = error_code;
    journal->recovery = phase == CUP_UPDATE_PHASE_FAILED ? CUP_UPDATE_FAILURE_PENDING
                                                         : CUP_UPDATE_FAILURE_NONE;
    return save_cup_update_journal(journal);
}

CupError cup_update_journal_set_recovery(CupUpdateJournal *journal,
                                         CupUpdateFailureRecovery recovery) {
    if (journal == NULL || journal->phase != CUP_UPDATE_PHASE_FAILED ||
        recovery == CUP_UPDATE_FAILURE_NONE ||
        strcmp(cup_update_failure_recovery_name(recovery), "invalid") == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    journal->recovery = recovery;
    return save_cup_update_journal(journal);
}

static CupError parse_nonnegative_int(const char *value, int *result) {
    char *end;
    long parsed;

    if (text_is_empty(value) || result == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < 0 || parsed > 255) {
        return CUP_ERR_TRANSACTION;
    }
    *result = (int)parsed;
    return CUP_OK;
}

/* Strict journal decoding. Unknown, duplicate or inconsistent fields preserve the journal as a
 * blocker. */
static CupError set_cup_update_field(CupUpdateJournal *journal,
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
        bit = FIELD_ERROR;
        err = parse_nonnegative_int(value, &journal->error_code);
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

CupError cup_update_journal_load(CupUpdateJournal *journal, CupUpdateJournalStatus *status) {
    CupUpdateJournal candidate;
    FILE *file;
    CupError err;
    char path[MAX_PATH_LEN];
    char line[CUP_UPDATE_JOURNAL_LINE_LEN];
    size_t line_number = 0;
    unsigned seen = 0;

    if (journal == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    cup_update_journal_init(journal);
    cup_update_journal_init(&candidate);
    *status = CUP_UPDATE_JOURNAL_MISSING;
    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? CUP_OK : CUP_ERR_TRANSACTION;
    }

    /* Reject unknown or duplicate fields rather than accepting a partial update journal. */
    while (1) {
        char key[64];
        char value[MAX_PATH_LEN];
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (!has_line) {
            break;
        }
        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err == CUP_OK) {
            err = set_cup_update_field(&candidate, key, value, &seen);
        }
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }

    if (fclose(file) != 0 || seen != JOURNAL_FIELDS ||
        !journal_fields_are_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    *journal = candidate;
    *status = CUP_UPDATE_JOURNAL_LOADED;
    return CUP_OK;
}

CupError cup_update_journal_get_staging_path(const CupUpdateJournal *journal,
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


/* cup update recovery. */
static CupError files_are_equal(const char *left, const char *right, int *equal) {
    char left_hash[SHA256_HEX_LENGTH + 1];
    char right_hash[SHA256_HEX_LENGTH + 1];
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

static CupError validate_preserved_binary(const char *staging, const CupUpdateAsset *asset) {
    char backup[MAX_PATH_LEN];
    SystemPathKind backup_kind;
    SystemPathKind destination_kind;
    CupError err;
    int equal;

    err = path_join(backup, sizeof(backup), staging, asset->backup_name);
    if (err == CUP_OK) {
        err = system_get_path_kind(backup, &backup_kind);
    }
    if (err == CUP_OK) {
        err = system_get_path_kind(asset->destination, &destination_kind);
    }
    if (err != CUP_OK || destination_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }
    if (backup_kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (backup_kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }

    err = files_are_equal(backup, asset->destination, &equal);
    return err == CUP_OK && equal ? CUP_OK : CUP_ERR_TRANSACTION;
}

/* Recovery after helper interruption. The committed marker decides whether backups are restored or
 * cleanup is completed. */
static CupError restore_cup_update_asset(const char *staging,
                                         const CupUpdateAsset *asset,
                                         int preserve_destination) {
    char backup[MAX_PATH_LEN];
    CupError err;
    SystemPathKind backup_kind = SYSTEM_PATH_MISSING;
    SystemPathKind destination_kind = SYSTEM_PATH_MISSING;
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;

    err = path_join(backup, sizeof(backup), staging, asset->backup_name);
    if (err == CUP_OK) {
        err = system_get_path_kind(backup, &backup_kind);
    }
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    err = system_get_path_kind(asset->destination, &destination_kind);
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (backup_kind == SYSTEM_PATH_MISSING) {
        return destination_kind == SYSTEM_PATH_REGULAR_FILE ? CUP_OK : CUP_ERR_TRANSACTION;
    }
    if (backup_kind != SYSTEM_PATH_REGULAR_FILE ||
        (destination_kind != SYSTEM_PATH_MISSING && destination_kind != SYSTEM_PATH_REGULAR_FILE)) {
        return CUP_ERR_TRANSACTION;
    }
    if (preserve_destination) {
        int equal;

        if (destination_kind != SYSTEM_PATH_REGULAR_FILE ||
            files_are_equal(backup, asset->destination, &equal) != CUP_OK || !equal) {
            return CUP_ERR_TRANSACTION;
        }
        err = filesystem_apply_required_permissions(
            asset->destination, asset->executable, asset->read_only);
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Error: could not restore permissions for CUP update asset '%s'.\n",
                    asset->destination);
            return CUP_ERR_TRANSACTION;
        }
        return CUP_OK;
    }
    if (destination_kind == SYSTEM_PATH_REGULAR_FILE &&
        system_set_read_only(asset->destination, 0) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    err = system_replace_file(backup, asset->destination, &state);
    if (err != CUP_OK) {
        return state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_ROLLBACK : CUP_ERR_COMMIT;
    }
    err = filesystem_apply_required_permissions(
        asset->destination, asset->executable, asset->read_only);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: could not restore permissions for CUP update asset '%s'.\n",
                asset->destination);
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

CupError cup_update_journal_recover(const CupUpdateJournal *journal,
                                    CupUpdateRecoveryMode mode,
                                    CupUpdateRecoveryResult *result) {
    CupAssetsInspection inspection;
    CupError err;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char platform_checksums[MAX_PATH_LEN];
    char catalog[MAX_PATH_LEN];
    char install_policy[MAX_PATH_LEN];
    char common_checksums[MAX_PATH_LEN];
    SystemPathKind marker_kind = SYSTEM_PATH_MISSING;
    SystemPathKind staging_kind = SYSTEM_PATH_MISSING;
    CupUpdateAsset assets[6];
    size_t i;

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
        err = cup_update_journal_get_staging_path(journal, staging, sizeof(staging));
        if (err == CUP_OK) {
            err = system_get_path_kind(staging, &staging_kind);
        }
        if (err != CUP_OK ||
            (staging_kind != SYSTEM_PATH_MISSING &&
             staging_kind != SYSTEM_PATH_DIRECTORY)) {
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
        err = cup_assets_inspect(&inspection);
        if (err != CUP_OK || !cup_assets_installed_is_valid(&inspection)) {
            return CUP_ERR_TRANSACTION;
        }
        err = runtime_journal_clear();
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

    /* Resolve canonical destinations and inspect the durable commit marker first. */
    err = cup_update_journal_get_staging_path(journal, staging, sizeof(staging));
    if (err == CUP_OK) {
        err = path_join(marker, sizeof(marker), staging, CUP_UPDATE_COMMITTED);
    }
    if (err == CUP_OK) {
        err = layout_get_binary_path(binary, sizeof(binary));
    }
    if (err == CUP_OK) {
        err = layout_get_uninstall_path(uninstall, sizeof(uninstall));
    }
    if (err == CUP_OK) {
        err = layout_get_platform_checksums_path(platform_checksums, sizeof(platform_checksums));
    }
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(catalog, sizeof(catalog));
    }
    if (err == CUP_OK) {
        err = layout_get_install_policy_path(install_policy, sizeof(install_policy));
    }
    if (err == CUP_OK) {
        err = layout_get_common_checksums_path(common_checksums, sizeof(common_checksums));
    }
    if (err == CUP_OK) {
        err = system_get_path_kind(marker, &marker_kind);
    }
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    /* Backups are restored in a fixed order when the generation did not commit cleanly. */
    assets[0] = (CupUpdateAsset){CUP_UPDATE_BINARY_OLD, binary, 1, 0};
    assets[1] = (CupUpdateAsset){
        CUP_UPDATE_UNINSTALL_OLD, uninstall, CUP_UNINSTALL_EXECUTABLE, 1};
    assets[2] = (CupUpdateAsset){CUP_UPDATE_PLATFORM_CHECKSUMS_OLD, platform_checksums, 0, 1};
    assets[3] = (CupUpdateAsset){CUP_UPDATE_PACKAGES_OLD, catalog, 0, 1};
    assets[4] = (CupUpdateAsset){CUP_UPDATE_INSTALL_POLICY_OLD, install_policy, 0, 1};
    assets[5] = (CupUpdateAsset){CUP_UPDATE_COMMON_CHECKSUMS_OLD, common_checksums, 0, 1};

    /* After the marker, a valid installed generation is finalized rather than rolled back. */
    if (marker_kind == SYSTEM_PATH_REGULAR_FILE) {
        err = cup_assets_inspect(&inspection);
        if (err == CUP_OK && cup_assets_installed_is_valid(&inspection)) {
            err = runtime_journal_clear();
            if (err != CUP_OK) {
                return err;
            }
            if (filesystem_remove_tree(staging) != CUP_OK) {
                fprintf(stderr,
                        "Warning: the cup update completed, but stale update staging could not "
                        "be removed. Run 'cup repair'.\n");
            }
            if (result != NULL) {
                *result = CUP_UPDATE_RECOVERY_FINALIZED;
            }
            printf("Completed interrupted cup update transaction.\n");
            return CUP_OK;
        }
    } else if (marker_kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }

    /* Repair must never replace its own running executable. Validate that the canonical binary is
     * already the backed-up generation before changing any supporting asset. */
    if (mode == CUP_UPDATE_RECOVER_PRESERVE_BINARY &&
        validate_preserved_binary(staging, &assets[0]) != CUP_OK) {
        fprintf(stderr,
                "Error: interrupted cup update recovery would replace the running executable. "
                "Repair preserved cup and all transaction evidence; run the official installer "
                "to recover safely.\n");
        return CUP_ERR_TRANSACTION;
    }

    /* Without a valid committed generation, restore supporting assets first and the binary last. */
    for (i = 1; i < sizeof(assets) / sizeof(assets[0]); ++i) {
        err = restore_cup_update_asset(staging, &assets[i], 0);
        if (err != CUP_OK) {
            return err;
        }
    }
    err = restore_cup_update_asset(
        staging, &assets[0], mode == CUP_UPDATE_RECOVER_PRESERVE_BINARY);
    if (err != CUP_OK) {
        return err;
    }

    err = cup_assets_inspect(&inspection);
    if (err != CUP_OK || !cup_assets_installed_is_valid(&inspection)) {
        return CUP_ERR_TRANSACTION;
    }

    if (journal->phase == CUP_UPDATE_PHASE_FAILED) {
        CupUpdateJournal recovered = *journal;

        err = cup_update_journal_set_recovery(&recovered, CUP_UPDATE_FAILURE_ROLLED_BACK);
        if (err != CUP_OK) {
            return err;
        }
        if (filesystem_remove_tree(staging) != CUP_OK) {
            fprintf(stderr,
                    "Warning: the failed cup update was rolled back, but stale staging could "
                    "not be removed. Run 'cup repair' again.\n");
        }
        printf("Rolled back failed cup update to version %s. Run 'cup repair' again to "
               "acknowledge the recorded failure.\n",
               journal->version);
    } else {
        err = runtime_journal_clear();
        if (err != CUP_OK) {
            return err;
        }
        if (filesystem_remove_tree(staging) != CUP_OK) {
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
