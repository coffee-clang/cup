/*
 * Persists, validates and recovers the deferred CUP update protocol. Package transactions remain
 * a separate journal owner.
 */

#include "cup_update_journal.h"

#include "cup_assets.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUP_UPDATE_JOURNAL_FORMAT "1"
#define CUP_UPDATE_RESULT_FORMAT "1"
#define CUP_UPDATE_JOURNAL_LINE_LEN 512
#define FIELD_FORMAT (1u << 0)
#define FIELD_OPERATION (1u << 1)
#define FIELD_PHASE (1u << 2)
#define FIELD_TEMPORARY_NAME (1u << 3)
#define FIELD_TOKEN (1u << 4)
#define FIELD_VERSION (1u << 5)
#define FIELD_ERROR (1u << 6)
#define JOURNAL_FIELDS \
    (FIELD_FORMAT | FIELD_OPERATION | FIELD_PHASE | FIELD_TEMPORARY_NAME | FIELD_TOKEN | \
     FIELD_VERSION | FIELD_ERROR)

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

static int temporary_name_is_valid(const char *name) {
    return path_is_safe_segment(name) && strncmp(name, "cup-update-", 11) == 0 && name[11] != '\0';
}

static int token_is_valid(const char *token) {
    size_t i;

    if (text_is_empty(token) || strlen(token) >= MAX_PATH_LEN) {
        return 0;
    }
    for (i = 0; token[i] != '\0'; ++i) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static CupError save_cup_update_journal(const CupUpdateJournal *journal) {
    CupError err;
    FILE *file = NULL;
    char path[MAX_PATH_LEN];
    char staging_dir[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int failed = 0;

    if (journal == NULL || !temporary_name_is_valid(journal->temporary_name) ||
        !token_is_valid(journal->token) ||
        strcmp(cup_update_phase_name(journal->phase), "invalid") == 0 ||
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
        fprintf(file, "error=%d\n", journal->error_code) < 0 || system_sync_file(file) != CUP_OK) {
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

    if (text_is_empty(temporary_path) || !token_is_valid(token) || text_is_empty(version) ||
        strlen(version) >= MAX_IDENTIFIER_LEN) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (exists) {
        fprintf(stderr, "Error: an interrupted CUP operation must be repaired first.\n");
        return CUP_ERR_TRANSACTION;
    }

    name = path_last_segment(temporary_path);
    if (!temporary_name_is_valid(name)) {
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
    if (journal == NULL || strcmp(cup_update_phase_name(phase), "invalid") == 0 || error_code < 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    journal->phase = phase;
    journal->error_code = error_code;
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
        if (strcmp(value, CUP_UPDATE_JOURNAL_FORMAT) != 0)
            err = CUP_ERR_TRANSACTION;
    } else if (strcmp(key, "operation") == 0) {
        bit = FIELD_OPERATION;
        if (strcmp(value, "cup-update") != 0)
            err = CUP_ERR_TRANSACTION;
    } else if (strcmp(key, "phase") == 0) {
        bit = FIELD_PHASE;
        if (!parse_phase(value, &journal->phase))
            err = CUP_ERR_TRANSACTION;
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

    while (1) {
        char key[64];
        char value[MAX_PATH_LEN];
        int has_line;

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (!has_line)
            break;
        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err == CUP_OK)
            err = set_cup_update_field(&candidate, key, value, &seen);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }

    if (fclose(file) != 0 || seen != JOURNAL_FIELDS ||
        !temporary_name_is_valid(candidate.temporary_name) || !token_is_valid(candidate.token) ||
        text_is_empty(candidate.version) ||
        (candidate.phase != CUP_UPDATE_PHASE_FAILED && candidate.error_code != 0)) {
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

CupError cup_update_journal_clear(void) {
    char path[MAX_PATH_LEN];
    int exists;

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (!exists)
        return CUP_OK;
    if (system_remove_file(path) != CUP_OK || system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

/* Persistent result file. The detached helper reports success or failure without relying on an
 * inherited terminal. */
void cup_update_result_init(CupUpdateResult *result) {
    if (result != NULL)
        memset(result, 0, sizeof(*result));
}

CupError cup_update_result_write(CupUpdateResultStatus status,
                                 int error_code,
                                 const char *version) {
    char path[MAX_PATH_LEN];
    char root[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    const char *status_name;
    int failed = 0;

    if ((status != CUP_UPDATE_RESULT_SUCCESS && status != CUP_UPDATE_RESULT_FAILED) ||
        error_code < 0 || text_is_empty(version)) {
        return CUP_ERR_INVALID_INPUT;
    }
    status_name = status == CUP_UPDATE_RESULT_SUCCESS ? "success" : "failed";
    if (layout_get_cup_update_result_path(path, sizeof(path)) != CUP_OK ||
        layout_get_root(root, sizeof(root)) != CUP_OK ||
        system_create_temp_file(root, "cup-update-result", temporary, sizeof(temporary), &file) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (fprintf(file, "format=%s\n", CUP_UPDATE_RESULT_FORMAT) < 0 ||
        fprintf(file, "status=%s\n", status_name) < 0 ||
        fprintf(file, "error=%d\n", error_code) < 0 || fprintf(file, "version=%s\n", version) < 0 ||
        system_sync_file(file) != CUP_OK)
        failed = 1;
    if (fclose(file) != 0)
        failed = 1;
    if (failed) {
        system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }
    err = system_replace_file(temporary, path, &commit_state);
    if (err == CUP_OK)
        return CUP_OK;
    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED)
        system_remove_file(temporary);
    return commit_state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_TRANSACTION : CUP_ERR_COMMIT;
}

CupError cup_update_result_load(CupUpdateResult *result) {
    char path[MAX_PATH_LEN];
    char line[256];
    FILE *file;
    size_t line_number = 0;
    unsigned seen = 0;

    if (result == NULL)
        return CUP_ERR_INVALID_INPUT;
    cup_update_result_init(result);
    if (layout_get_cup_update_result_path(path, sizeof(path)) != CUP_OK)
        return CUP_ERR_TRANSACTION;
    file = fopen(path, "r");
    if (file == NULL)
        return errno == ENOENT ? CUP_OK : CUP_ERR_TRANSACTION;

    while (1) {
        char key[64];
        char value[128];
        int has_line;
        CupError err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        unsigned bit;
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (!has_line)
            break;
        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (strcmp(key, "format") == 0) {
            bit = 1u << 0;
            if (strcmp(value, CUP_UPDATE_RESULT_FORMAT) != 0) {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
        } else if (strcmp(key, "status") == 0) {
            bit = 1u << 1;
            if (strcmp(value, "success") == 0)
                result->status = CUP_UPDATE_RESULT_SUCCESS;
            else if (strcmp(value, "failed") == 0)
                result->status = CUP_UPDATE_RESULT_FAILED;
            else {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
        } else if (strcmp(key, "error") == 0) {
            bit = 1u << 2;
            if (parse_nonnegative_int(value, &result->error_code) != CUP_OK) {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
        } else if (strcmp(key, "version") == 0) {
            bit = 1u << 3;
            if (text_copy(result->version, sizeof(result->version), value) != CUP_OK) {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
        } else {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if ((seen & bit) != 0) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        seen |= bit;
    }
    if (fclose(file) != 0 || seen != 0xfu || result->status == CUP_UPDATE_RESULT_MISSING ||
        text_is_empty(result->version) ||
        (result->status == CUP_UPDATE_RESULT_SUCCESS && result->error_code != 0)) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

CupError cup_update_result_report(void) {
    CupUpdateResult result;
    CupError err = cup_update_result_load(&result);
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: the previous CUP update result is invalid. Run 'cup doctor'.\n");
        return err;
    }
    if (result.status == CUP_UPDATE_RESULT_SUCCESS) {
        printf("Info: the previous CUP update completed successfully at version %s.\n",
               result.version);
    } else if (result.status == CUP_UPDATE_RESULT_FAILED) {
        fprintf(stderr,
                "Warning: the previous CUP update failed (error %d). Run 'cup doctor' and 'cup "
                "repair'.\n",
                result.error_code);
    }
    return CUP_OK;
}

/* CUP update recovery. */
static CupError set_asset_permissions(const CupUpdateAsset *asset) {
    CupError err;

    if (asset->executable) {
        err = system_set_executable(asset->destination, 1);
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
    }
    if (asset->read_only) {
        err = system_set_read_only(asset->destination, 1);
        if (err != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
    }
    return CUP_OK;
}

/* Recovery after helper interruption. The committed marker decides whether backups are restored or
 * cleanup is completed. */
static CupError restore_cup_update_asset(const char *staging, const CupUpdateAsset *asset) {
    char backup[MAX_PATH_LEN];
    CupError err;
    SystemPathKind backup_kind;
    SystemPathKind destination_kind;
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
    if (destination_kind == SYSTEM_PATH_REGULAR_FILE &&
        system_set_read_only(asset->destination, 0) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    err = system_replace_file(backup, asset->destination, &state);
    if (err != CUP_OK) {
        return state == SYSTEM_COMMIT_NOT_APPLIED ? CUP_ERR_ROLLBACK : CUP_ERR_COMMIT;
    }
    if (set_asset_permissions(asset) != CUP_OK) {
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

CupError cup_update_journal_recover(const CupUpdateJournal *journal) {
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
    CupUpdateAsset assets[6];
    size_t i;

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

    assets[0] = (CupUpdateAsset){CUP_UPDATE_BINARY_OLD, binary, 1, 0};
    assets[1] = (CupUpdateAsset){CUP_UPDATE_UNINSTALL_OLD, uninstall, 1, 1};
    assets[2] = (CupUpdateAsset){CUP_UPDATE_PLATFORM_CHECKSUMS_OLD, platform_checksums, 0, 1};
    assets[3] = (CupUpdateAsset){CUP_UPDATE_PACKAGES_OLD, catalog, 0, 1};
    assets[4] = (CupUpdateAsset){CUP_UPDATE_INSTALL_POLICY_OLD, install_policy, 0, 1};
    assets[5] = (CupUpdateAsset){CUP_UPDATE_COMMON_CHECKSUMS_OLD, common_checksums, 0, 1};

    if (marker_kind == SYSTEM_PATH_REGULAR_FILE) {
        err = cup_assets_inspect(&inspection);
        if (err == CUP_OK && cup_assets_installed_is_valid(&inspection)) {
            err = cup_update_journal_clear();
            if (err == CUP_OK) {
                err = filesystem_remove_tree(staging);
            }
            if (err == CUP_OK) {
                printf("Completed interrupted cup update transaction.\n");
            }
            return err;
        }
    } else if (marker_kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }

    for (i = 0; i < sizeof(assets) / sizeof(assets[0]); ++i) {
        err = restore_cup_update_asset(staging, &assets[i]);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = cup_assets_inspect(&inspection);
    if (err != CUP_OK || !cup_assets_installed_is_valid(&inspection)) {
        return CUP_ERR_TRANSACTION;
    }

    err = cup_update_journal_clear();
    if (err == CUP_OK) {
        err = filesystem_remove_tree(staging);
    }
    if (err == CUP_OK) {
        printf("Rolled back interrupted cup update transaction.\n");
    }
    return err;
}
