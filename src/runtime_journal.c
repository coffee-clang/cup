/*
 * Classifies the shared transaction.txt file and emits the common blocking diagnostic used before
 * normal commands open mutable state.
 */

#include "runtime_journal.h"

#include "filesystem.h"
#include "layout.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

static CupError runtime_journal_read(PersistentFileSnapshot *snapshot, int *missing) {
    char path[MAX_PATH_LEN];

    if (snapshot == NULL || missing == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    filesystem_snapshot_release(snapshot);
    *missing = 0;
    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return filesystem_snapshot_read(
        path, MAX_RUNTIME_JOURNAL_BYTES, snapshot, missing) == CUP_OK
               ? CUP_OK
               : CUP_ERR_TRANSACTION;
}

CupError runtime_journal_parse(const char *const *ordered_keys,
                               size_t ordered_key_count,
                               RuntimeJournalFieldVisitor visitor,
                               void *userdata,
                               SystemPathIdentity *identity,
                               int *missing) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    char line[MAX_METADATA_LINE_LEN];
    CupError err;

    if (identity != NULL) {
        memset(identity, 0, sizeof(*identity));
    }
    if (missing != NULL) {
        *missing = 0;
    }
    if (visitor == NULL || identity == NULL || missing == NULL ||
        ((ordered_keys == NULL) != (ordered_key_count == 0))) {
        return CUP_ERR_INVALID_INPUT;
    }

    filesystem_snapshot_init(&snapshot);

    err = runtime_journal_read(&snapshot, missing);
    if (err != CUP_OK || *missing) {
        return err;
    }
    if (memchr(snapshot.data, ' ', snapshot.size) != NULL ||
        text_document_reader_init(&reader, snapshot.data, snapshot.size) != CUP_OK) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_TRANSACTION;
    }

    while (1) {
        char key[64];
        char value[MAX_PATH_LEN];
        int has_line;

        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
        if (err != CUP_OK) {
            break;
        }
        if (!has_line) {
            break;
        }

        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err == CUP_OK && ordered_keys != NULL) {
            size_t index = reader.line_number - 1u;

            if (index >= ordered_key_count || strcmp(key, ordered_keys[index]) != 0) {
                err = CUP_ERR_TRANSACTION;
            }
        }
        if (err == CUP_OK) {
            err = visitor(key, value, userdata);
        }
        if (err != CUP_OK) {
            break;
        }
    }

    if (err == CUP_OK && ordered_keys != NULL && reader.line_number != ordered_key_count) {
        err = CUP_ERR_TRANSACTION;
    }
    if (err == CUP_OK) {
        *identity = snapshot.identity;
    }
    filesystem_snapshot_release(&snapshot);
    return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

CupError runtime_journal_publish(const char *temporary_directory,
                                 const char *temporary_prefix,
                                 const SystemPathIdentity *expected_identity,
                                 RuntimeJournalWriter writer,
                                 const void *value,
                                 SystemPathIdentity *published_identity) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    SystemPathIdentity source_identity;
    SystemPathIdentity destination_identity;
    char path[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;
    int close_failed = 0;

    if (published_identity != NULL) {
        memset(published_identity, 0, sizeof(*published_identity));
    }
    if (text_is_empty(temporary_directory) || text_is_empty(temporary_prefix) ||
        writer == NULL || published_identity == NULL ||
        (expected_identity != NULL &&
         (!expected_identity->valid ||
          expected_identity->kind != SYSTEM_PATH_REGULAR_FILE)) ||
        layout_get_transaction_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    memset(&source_identity, 0, sizeof(source_identity));
    memset(&destination_identity, 0, sizeof(destination_identity));

    err = system_create_temp_file(temporary_directory,
                                  temporary_prefix,
                                  temporary,
                                  sizeof(temporary),
                                  &file);
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    err = writer(file, value);
    if (err == CUP_OK) {
        err = system_sync_file(file);
    }
    if (fclose(file) != 0) {
        close_failed = 1;
    }
    if (err != CUP_OK || close_failed) {
        (void)system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }

    err = system_get_path_identity(temporary, &source_identity);
    if (err != CUP_OK || !source_identity.valid ||
        source_identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        (void)system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }

    if (expected_identity == NULL) {
        err = system_move_path(temporary, path, &commit_state);
    } else {
        err = system_replace_file_if_identity(
            temporary, path, expected_identity, &commit_state);
    }
    if (err == CUP_OK) {
        err = system_get_path_identity(path, &destination_identity);
        if (err != CUP_OK ||
            !system_path_identity_equal(&source_identity, &destination_identity)) {
            return CUP_ERR_COMMIT;
        }
        *published_identity = destination_identity;
        return CUP_OK;
    }

    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        (void)system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }

    if (system_get_path_identity(path, &destination_identity) == CUP_OK &&
        system_path_identity_equal(&source_identity, &destination_identity)) {
        *published_identity = destination_identity;
    }
    return CUP_ERR_COMMIT;
}

/* Central command policy for pending package transactions and cup-update journals. */
typedef struct {
    RuntimeJournalKind detected;
    int operation_seen;
} RuntimeJournalDetection;

static CupError detect_journal_field(const char *key,
                                     const char *value,
                                     void *userdata) {
    RuntimeJournalDetection *detection = userdata;

    if (strcmp(key, "operation") != 0) {
        return CUP_OK;
    }
    if (detection->operation_seen) {
        return CUP_ERR_TRANSACTION;
    }

    detection->operation_seen = 1;
    if (strcmp(value, "install") == 0 || strcmp(value, "remove") == 0 ||
        strcmp(value, "update") == 0) {
        detection->detected = RUNTIME_JOURNAL_PACKAGE;
    } else if (strcmp(value, "cup-update") == 0) {
        detection->detected = RUNTIME_JOURNAL_CUP_UPDATE;
    } else if (strcmp(value, "uninstall") == 0) {
        detection->detected = RUNTIME_JOURNAL_UNINSTALL;
    } else {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

CupError runtime_journal_detect(RuntimeJournalKind *kind) {
    RuntimeJournalDetection detection;
    SystemPathIdentity identity;
    CupError err;
    int missing;

    if (kind == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *kind = RUNTIME_JOURNAL_MISSING;
    memset(&detection, 0, sizeof(detection));
    detection.detected = RUNTIME_JOURNAL_MISSING;

    err = runtime_journal_parse(
        NULL, 0, detect_journal_field, &detection, &identity, &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (!detection.operation_seen) {
        return CUP_ERR_TRANSACTION;
    }

    *kind = detection.detected;
    return CUP_OK;
}

CupError runtime_journal_clear_if_identity(const SystemPathIdentity *expected_identity) {
    char path[MAX_PATH_LEN];
    CupError err;

    if (expected_identity == NULL || !expected_identity->valid ||
        expected_identity->kind != SYSTEM_PATH_REGULAR_FILE ||
        layout_get_transaction_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    err = system_remove_file_if_identity(path, expected_identity);
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return system_sync_parent_directory(path) == CUP_OK ? CUP_OK : CUP_ERR_COMMIT;
}

CupError runtime_journal_require_none(void) {
    RuntimeJournalKind kind;
    CupError err = runtime_journal_detect(&kind);

    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: transaction journal is invalid. "
                "Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_TRANSACTION;
    }
    if (kind == RUNTIME_JOURNAL_MISSING) {
        return CUP_OK;
    }
    if (kind == RUNTIME_JOURNAL_CUP_UPDATE) {
        fprintf(stderr,
                "Error: a cup update is pending or failed; retry shortly or run 'cup repair'.\n");
    } else if (kind == RUNTIME_JOURNAL_UNINSTALL) {
        fprintf(stderr,
                "Error: a cup uninstall is pending or failed; retry shortly or run "
                "'cup repair'.\n");
    } else {
        fprintf(stderr,
                "Error: a package transaction is active or requires recovery; "
                "retry shortly or run 'cup repair'.\n");
    }
    return CUP_ERR_TRANSACTION;
}
