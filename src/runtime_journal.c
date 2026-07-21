/*
 * Classifies the shared physical runtime journal and emits the common blocking diagnostic used by
 * normal commands before they open mutable state.
 */

#include "runtime_journal.h"

#include "layout.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RUNTIME_JOURNAL_LINE_LEN 512

/* Central command policy for pending package transactions and CUP-update journals. */
CupError runtime_journal_detect(RuntimeJournalKind *kind) {
    FILE *file;
    CupError err;
    char path[MAX_PATH_LEN];
    char line[RUNTIME_JOURNAL_LINE_LEN];
    size_t line_number = 0;
    int operation_seen = 0;
    RuntimeJournalKind detected = RUNTIME_JOURNAL_MISSING;

    if (kind == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *kind = RUNTIME_JOURNAL_MISSING;
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
        if (!has_line) {
            break;
        }
        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (strcmp(key, "operation") == 0) {
            if (operation_seen) {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
            operation_seen = 1;
            if (strcmp(value, "install") == 0 || strcmp(value, "remove") == 0 ||
                strcmp(value, "update") == 0) {
                detected = RUNTIME_JOURNAL_PACKAGE;
            } else if (strcmp(value, "cup-update") == 0) {
                detected = RUNTIME_JOURNAL_CUP_UPDATE;
            } else {
                fclose(file);
                return CUP_ERR_TRANSACTION;
            }
        }
    }

    if (fclose(file) != 0 || !operation_seen) {
        return CUP_ERR_TRANSACTION;
    }
    *kind = detected;
    return CUP_OK;
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
    fprintf(stderr,
            kind == RUNTIME_JOURNAL_CUP_UPDATE
                ? "Error: a CUP update is pending or failed; retry shortly or run 'cup repair'.\n"
                : "Error: an interrupted package transaction must be repaired first.\n");
    return CUP_ERR_TRANSACTION;
}
