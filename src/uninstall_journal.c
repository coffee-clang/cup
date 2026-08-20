/* Persists and validates the detached uninstall protocol in the shared transaction.txt file. */

#include "uninstall_journal.h"

#include "layout.h"
#include "path.h"
#include "filesystem.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define UNINSTALL_JOURNAL_FORMAT "1"
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

static const char *temporary_name_token(const char *name) {
    if (!path_is_safe_segment(name) || strlen(name) >= MAX_METADATA_VALUE_LEN ||
        strncmp(name, ".cup-uninstall", 14) != 0 ||
        (name[14] != '.' && name[14] != '-') || name[15] == '\0') {
        return NULL;
    }
    return name + 15;
}

static int token_is_valid(const char *token) {
    const unsigned char *cursor;

    if (text_is_empty(token) || strlen(token) >= MAX_TRANSACTION_TOKEN_LEN) {
        return 0;
    }
    for (cursor = (const unsigned char *)token; *cursor != '\0'; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' || *cursor == '-' ||
              *cursor == '.')) {
            return 0;
        }
    }
    return 1;
}

static int name_matches_token(const char *name, const char *token) {
    const char *name_token = temporary_name_token(name);

    return name_token != NULL && token_is_valid(token) && strcmp(name_token, token) == 0;
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

static CupError write_uninstall_journal(FILE *file, const void *value) {
    const UninstallJournal *journal = value;

    if (journal == NULL ||
        fprintf(file, "format=%s\n", UNINSTALL_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=uninstall\n") < 0 ||
        fprintf(file, "phase=%s\n", uninstall_phase_name(journal->phase)) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0 ||
        fprintf(file, "token=%s\n", journal->token) < 0 ||
        fprintf(file, "stage=%s\n", uninstall_stage_name(journal->stage)) < 0 ||
        fprintf(file, "error=%d\n", journal->error_code) < 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError save_journal(UninstallJournal *journal) {
    char root[MAX_PATH_LEN];
    SystemPathIdentity published_identity;
    const SystemPathIdentity *expected_identity;
    CupError err;

    if (!journal_is_coherent(journal) ||
        layout_get_root(root, sizeof(root)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    expected_identity = journal->file_identity.valid ? &journal->file_identity : NULL;
    err = runtime_journal_publish(root,
                                  "uninstall-transaction",
                                  expected_identity,
                                  write_uninstall_journal,
                                  journal,
                                  &published_identity);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && published_identity.valid) {
        journal->file_identity = published_identity;
    }
    return err;
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
        unsigned parsed;

        bit = FIELD_ERROR;
        if (!text_parse_uint(value, 255u, &parsed)) {
            err = CUP_ERR_TRANSACTION;
        } else {
            journal->error_code = (int)parsed;
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

static const char *const uninstall_journal_keys[] = {
    "format", "operation", "phase", "temporary_name",
    "token", "stage", "error"};

typedef struct {
    UninstallJournal *candidate;
    unsigned seen;
} UninstallJournalParser;

static CupError parse_uninstall_journal_field(const char *key,
                                              const char *value,
                                              void *userdata) {
    UninstallJournalParser *parser = userdata;

    if (parser == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    return set_field(parser->candidate, key, value, &parser->seen);
}

CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status) {
    UninstallJournal candidate;
    UninstallJournalParser parser;
    SystemPathIdentity file_identity;
    CupError err;
    int missing;

    if (journal == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    uninstall_journal_init(journal);
    uninstall_journal_init(&candidate);
    memset(&parser, 0, sizeof(parser));
    memset(&file_identity, 0, sizeof(file_identity));
    parser.candidate = &candidate;
    *status = UNINSTALL_JOURNAL_MISSING;

    err = runtime_journal_parse(uninstall_journal_keys,
                                sizeof(uninstall_journal_keys) /
                                    sizeof(uninstall_journal_keys[0]),
                                parse_uninstall_journal_field,
                                &parser,
                                &file_identity,
                                &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (parser.seen != JOURNAL_FIELDS ||
        !journal_is_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.file_identity = file_identity;

    *journal = candidate;
    *status = UNINSTALL_JOURNAL_LOADED;
    return CUP_OK;
}

static CupError uninstall_journal_get_detached_path(const UninstallJournal *journal,
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

CupError uninstall_journal_recover(const UninstallJournal *journal) {
    char detached[MAX_PATH_LEN];
    SystemPathKind kind;
    CupError err;

    if (journal == NULL || !journal_is_coherent(journal)) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = uninstall_journal_get_detached_path(journal, detached, sizeof(detached));
    if (err == CUP_OK) {
        err = system_get_path_kind(detached, &kind);
    }
    if (err != CUP_OK || kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }
    err = runtime_journal_clear_if_identity(&journal->file_identity);
    if (err == CUP_OK) {
        if (journal->phase == UNINSTALL_PHASE_FAILED) {
            printf("Acknowledged failed cup uninstall during '%s' (error %d).\n",
                   uninstall_stage_name(journal->stage),
                   journal->error_code);
        } else {
            printf("Cancelled interrupted cup uninstall in phase '%s' during '%s'.\n",
                   uninstall_phase_name(journal->phase),
                   uninstall_stage_name(journal->stage));
        }
    }
    return err;
}
