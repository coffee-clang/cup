/* Persists and validates the detached uninstall protocol in the shared transaction.txt file. */

#include "uninstall_journal.h"

#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNINSTALL_JOURNAL_FORMAT "1"
#define UNINSTALL_JOURNAL_LINE_LEN 512
#define FIELD_FORMAT (1u << 0)
#define FIELD_OPERATION (1u << 1)
#define FIELD_PHASE (1u << 2)
#define FIELD_TEMPORARY_NAME (1u << 3)
#define FIELD_TOKEN (1u << 4)
#define FIELD_STAGE (1u << 5)
#define FIELD_ERROR (1u << 6)
#define JOURNAL_FIELDS \
    (FIELD_FORMAT | FIELD_OPERATION | FIELD_PHASE | FIELD_TEMPORARY_NAME | FIELD_TOKEN | \
     FIELD_STAGE | FIELD_ERROR)

void uninstall_journal_init(UninstallJournal *journal) {
    if (journal != NULL) {
        memset(journal, 0, sizeof(*journal));
        journal->phase = UNINSTALL_PHASE_SCHEDULED;
        journal->stage = UNINSTALL_STAGE_HANDOFF;
    }
}

const char *uninstall_phase_name(UninstallPhase phase) {
    switch (phase) {
        case UNINSTALL_PHASE_SCHEDULED:
            return "scheduled";
        case UNINSTALL_PHASE_DETACHING:
            return "detaching";
        case UNINSTALL_PHASE_FAILED:
            return "failed";
        default:
            return "invalid";
    }
}

const char *uninstall_stage_name(UninstallStage stage) {
    switch (stage) {
        case UNINSTALL_STAGE_HANDOFF:
            return "handoff";
        case UNINSTALL_STAGE_PARENT_WAIT:
            return "parent-wait";
        case UNINSTALL_STAGE_DETACH:
            return "detach";
        case UNINSTALL_STAGE_CLEANUP:
            return "cleanup";
        default:
            return "invalid";
    }
}

static int parse_phase(const char *value, UninstallPhase *phase) {
    if (strcmp(value, "scheduled") == 0) {
        *phase = UNINSTALL_PHASE_SCHEDULED;
    } else if (strcmp(value, "detaching") == 0) {
        *phase = UNINSTALL_PHASE_DETACHING;
    } else if (strcmp(value, "failed") == 0) {
        *phase = UNINSTALL_PHASE_FAILED;
    } else {
        return 0;
    }
    return 1;
}

static int parse_stage(const char *value, UninstallStage *stage) {
    if (strcmp(value, "handoff") == 0) {
        *stage = UNINSTALL_STAGE_HANDOFF;
    } else if (strcmp(value, "parent-wait") == 0) {
        *stage = UNINSTALL_STAGE_PARENT_WAIT;
    } else if (strcmp(value, "detach") == 0) {
        *stage = UNINSTALL_STAGE_DETACH;
    } else if (strcmp(value, "cleanup") == 0) {
        *stage = UNINSTALL_STAGE_CLEANUP;
    } else {
        return 0;
    }
    return 1;
}

static int temporary_name_is_valid(const char *name) {
    return path_is_safe_segment(name) && strncmp(name, ".cup-uninstall.", 15) == 0 &&
           name[15] != '\0';
}

static int token_is_valid(const char *token) {
    const unsigned char *cursor;

    if (text_is_empty(token) || strlen(token) >= MAX_IDENTIFIER_LEN) {
        return 0;
    }
    for (cursor = (const unsigned char *)token; *cursor != '\0'; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-')) {
            return 0;
        }
    }
    return 1;
}

static int name_matches_token(const char *name, const char *token) {
    return temporary_name_is_valid(name) && token_is_valid(token) && strcmp(name + 15, token) == 0;
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

static int journal_is_coherent(const UninstallJournal *journal) {
    if (journal == NULL || !name_matches_token(journal->temporary_name, journal->token) ||
        strcmp(uninstall_phase_name(journal->phase), "invalid") == 0 ||
        strcmp(uninstall_stage_name(journal->stage), "invalid") == 0) {
        return 0;
    }
    switch (journal->phase) {
        case UNINSTALL_PHASE_SCHEDULED:
            return journal->error_code == 0 &&
                   (journal->stage == UNINSTALL_STAGE_HANDOFF ||
                    journal->stage == UNINSTALL_STAGE_PARENT_WAIT);
        case UNINSTALL_PHASE_DETACHING:
            return journal->error_code == 0 && journal->stage == UNINSTALL_STAGE_DETACH;
        case UNINSTALL_PHASE_FAILED:
            return journal->error_code > 0;
        default:
            return 0;
    }
}

static CupError save_journal(const UninstallJournal *journal) {
    char root[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;
    int failed = 0;

    if (!journal_is_coherent(journal) || layout_get_root(root, sizeof(root)) != CUP_OK ||
        layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_create_temp_file(root, "uninstall-transaction", temporary, sizeof(temporary), &file) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (fprintf(file, "format=%s\n", UNINSTALL_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=uninstall\n") < 0 ||
        fprintf(file, "phase=%s\n", uninstall_phase_name(journal->phase)) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0 ||
        fprintf(file, "token=%s\n", journal->token) < 0 ||
        fprintf(file, "stage=%s\n", uninstall_stage_name(journal->stage)) < 0 ||
        fprintf(file, "error=%d\n", journal->error_code) < 0 ||
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

CupError uninstall_journal_begin(const char *temporary_path, const char *token) {
    UninstallJournal journal;
    RuntimeJournalKind kind;
    const char *name;
    CupError err;

    if (text_is_empty(temporary_path) || !token_is_valid(token)) {
        return CUP_ERR_INVALID_INPUT;
    }
    name = path_last_segment(temporary_path);
    if (!name_matches_token(name, token)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = runtime_journal_detect(&kind);
    if (err != CUP_OK || kind != RUNTIME_JOURNAL_MISSING) {
        return CUP_ERR_TRANSACTION;
    }

    uninstall_journal_init(&journal);
    if (text_copy(journal.temporary_name, sizeof(journal.temporary_name), name) != CUP_OK ||
        text_copy(journal.token, sizeof(journal.token), token) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return save_journal(&journal);
}

static CupError set_field(UninstallJournal *journal,
                          const char *key,
                          const char *value,
                          unsigned *seen) {
    unsigned bit;
    CupError err = CUP_OK;

    if (strcmp(key, "format") == 0) {
        bit = FIELD_FORMAT;
        if (strcmp(value, UNINSTALL_JOURNAL_FORMAT) != 0) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        bit = FIELD_OPERATION;
        if (strcmp(value, "uninstall") != 0) {
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
    } else if (strcmp(key, "stage") == 0) {
        bit = FIELD_STAGE;
        if (!parse_stage(value, &journal->stage)) {
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

CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status) {
    UninstallJournal candidate;
    char path[MAX_PATH_LEN];
    char line[UNINSTALL_JOURNAL_LINE_LEN];
    FILE *file;
    size_t line_number = 0;
    unsigned seen = 0;

    if (journal == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    uninstall_journal_init(journal);
    uninstall_journal_init(&candidate);
    *status = UNINSTALL_JOURNAL_MISSING;
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
        CupError err = text_read_line(file, line, sizeof(line), &has_line, &line_number);

        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (!has_line) {
            break;
        }
        err = text_parse_key_value(line, key, sizeof(key), value, sizeof(value));
        if (err == CUP_OK) {
            err = set_field(&candidate, key, value, &seen);
        }
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }
    if (fclose(file) != 0 || seen != JOURNAL_FIELDS || !journal_is_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    *journal = candidate;
    *status = UNINSTALL_JOURNAL_LOADED;
    return CUP_OK;
}

CupError uninstall_journal_get_detached_path(const UninstallJournal *journal,
                                             char *buffer,
                                             size_t size) {
    char root[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    CupError err;

    if (journal == NULL || buffer == NULL || size == 0 || !journal_is_coherent(journal)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_root(root, sizeof(root));
    if (err == CUP_OK) {
        err = path_parent(parent, sizeof(parent), root);
    }
    return err == CUP_OK ? path_join(buffer, size, parent, journal->temporary_name) : err;
}

CupError uninstall_journal_acknowledge_failure(const UninstallJournal *journal) {
    char detached[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    if (journal == NULL || journal->phase != UNINSTALL_PHASE_FAILED) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = uninstall_journal_get_detached_path(journal, detached, sizeof(detached));
    if (err == CUP_OK) {
        err = system_get_path_kind(detached, &kind);
    }
    if (err != CUP_OK || kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }
    err = runtime_journal_clear();
    if (err == CUP_OK) {
        printf("Acknowledged failed cup uninstall during '%s' (error %d).\n",
               uninstall_stage_name(journal->stage),
               journal->error_code);
    }
    return err;
}
