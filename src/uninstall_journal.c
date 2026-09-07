/* Persists and validates the detached uninstall protocol in the shared transaction.txt file. */

#include "uninstall_journal.h"

#include "exit_status.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define UNINSTALL_JOURNAL_FORMAT "2"
void uninstall_journal_init(UninstallJournal *journal) {
    if (journal != NULL) {
        memset(journal, 0, sizeof(*journal));
        journal->phase = UNINSTALL_PHASE_SCHEDULED;
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

static const char *temporary_name_token(const char *name) {
    if (name == NULL || strlen(name) >= MAX_METADATA_VALUE_LEN) {
        return NULL;
    }
    return path_generated_temp_suffix(name, CUP_UNINSTALL_TEMP_PREFIX);
}

static int name_matches_token(const char *name, const char *token) {
    const char *name_token = temporary_name_token(name);

    return name_token != NULL && runtime_journal_token_is_valid(token) && strcmp(name_token, token) == 0;
}

static int journal_is_coherent(const UninstallJournal *journal) {
    if (journal == NULL || !name_matches_token(journal->temporary_name, journal->token) ||
        strcmp(uninstall_phase_name(journal->phase), "invalid") == 0) {
        return 0;
    }
    switch (journal->phase) {
        case UNINSTALL_PHASE_SCHEDULED:
        case UNINSTALL_PHASE_DETACHING:
            return journal->error_code == 0;
        case UNINSTALL_PHASE_FAILED:
            return journal->error_code == CUP_STATUS_OPERATION;
        default:
            return 0;
    }
}

static CupError write_journal(FILE *file, const void *value) {
    const UninstallJournal *journal = value;

    if (journal == NULL ||
        fprintf(file, "format=%s\n", UNINSTALL_JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=%s\n", CUP_UNINSTALL_JOURNAL_OPERATION) < 0 ||
        fprintf(file, "phase=%s\n", uninstall_phase_name(journal->phase)) < 0 ||
        fprintf(file, "temporary_name=%s\n", journal->temporary_name) < 0 ||
        fprintf(file, "token=%s\n", journal->token) < 0 ||
        fprintf(file, "error=%d\n", journal->error_code) < 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError save_journal_at(const char *root, UninstallJournal *journal) {
    SystemPathIdentity published_identity;
    const SystemPathIdentity *expected_identity;
    CupError err;

    if (text_is_empty(root) || !journal_is_coherent(journal)) {
        return CUP_ERR_TRANSACTION;
    }

    expected_identity = journal->file_identity.valid ? &journal->file_identity : NULL;
    err = runtime_journal_publish_at(root,
                                     root,
                                     "uninstall-transaction",
                                     expected_identity,
                                     write_journal,
                                     journal,
                                     &published_identity);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && published_identity.valid) {
        journal->file_identity = published_identity;
    }
    return err;
}

static CupError save_journal(UninstallJournal *journal) {
    char root[MAX_PATH_LEN];
    CupError err = layout_get_root(root, sizeof(root));

    return err == CUP_OK ? save_journal_at(root, journal) : err;
}

CupError uninstall_journal_begin(const char *temporary_path, const char *token) {
    UninstallJournal journal;
    RuntimeJournalKind kind;
    const char *name;
    CupError err;

    if (text_is_empty(temporary_path) || !runtime_journal_token_is_valid(token)) {
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
                          const char *value) {
    CupError err = CUP_OK;

    if (strcmp(key, "format") == 0) {
        if (strcmp(value, UNINSTALL_JOURNAL_FORMAT) != 0) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        if (strcmp(value, CUP_UNINSTALL_JOURNAL_OPERATION) != 0) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "phase") == 0) {
        if (!parse_phase(value, &journal->phase)) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "temporary_name") == 0) {
        if (text_copy(journal->temporary_name, sizeof(journal->temporary_name), value) != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "token") == 0) {
        if (text_copy(journal->token, sizeof(journal->token), value) != CUP_OK) {
            err = CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "error") == 0) {
        unsigned parsed;

        if (!text_parse_uint(value, 255u, &parsed)) {
            err = CUP_ERR_TRANSACTION;
        } else {
            journal->error_code = (int)parsed;
        }
    } else {
        return CUP_ERR_TRANSACTION;
    }
    return err == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static const char *const journal_keys[] = {
    "format", "operation", "phase", "temporary_name", "token", "error"};

static CupError parse_field(const char *key,
                            const char *value,
                            void *userdata) {
    UninstallJournal *candidate = userdata;

    if (candidate == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    return set_field(candidate, key, value);
}

CupError uninstall_journal_load_at(const char *root,
                                   UninstallJournal *journal,
                                   UninstallJournalStatus *status) {
    UninstallJournal candidate;
    SystemPathIdentity file_identity;
    CupError err;
    int missing;

    if (text_is_empty(root) || journal == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    uninstall_journal_init(journal);
    uninstall_journal_init(&candidate);
    memset(&file_identity, 0, sizeof(file_identity));
    *status = UNINSTALL_JOURNAL_MISSING;

    err = runtime_journal_parse_at(root,
                                   journal_keys,
                                   sizeof(journal_keys) /
                                       sizeof(journal_keys[0]),
                                   parse_field,
                                   &candidate,
                                   &file_identity,
                                   &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (!journal_is_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.file_identity = file_identity;

    *journal = candidate;
    *status = UNINSTALL_JOURNAL_LOADED;
    return CUP_OK;
}

CupError uninstall_journal_load(UninstallJournal *journal, UninstallJournalStatus *status) {
    char root[MAX_PATH_LEN];
    CupError err = layout_get_root(root, sizeof(root));

    return err == CUP_OK ? uninstall_journal_load_at(root, journal, status) : err;
}

CupError uninstall_journal_set_at(const char *root,
                                  UninstallJournal *journal,
                                  UninstallPhase phase,
                                  int error_code) {
    UninstallJournal candidate;
    CupError err;

    if (text_is_empty(root) || journal == NULL || !journal->file_identity.valid) {
        return CUP_ERR_INVALID_INPUT;
    }
    candidate = *journal;
    candidate.phase = phase;
    candidate.error_code = error_code;
    if (!journal_is_coherent(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    err = save_journal_at(root, &candidate);
    if (err == CUP_OK || err == CUP_ERR_COMMIT) {
        *journal = candidate;
    }
    return err;
}

static CupError get_detached_path(const UninstallJournal *journal,
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
    err = get_detached_path(journal, detached, sizeof(detached));
    if (err == CUP_OK) {
        err = system_get_path_kind(detached, &kind);
    }
    if (err != CUP_OK || kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }
    err = runtime_journal_clear_if_identity(&journal->file_identity);
    if (err == CUP_OK) {
        if (journal->phase == UNINSTALL_PHASE_FAILED) {
            printf("Acknowledged failed cup uninstall (error %d).\n", journal->error_code);
        } else {
            printf("Cancelled interrupted cup uninstall in phase '%s'.\n",
                   uninstall_phase_name(journal->phase));
        }
    }
    return err;
}
