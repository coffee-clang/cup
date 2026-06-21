#include "transaction.h"

#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define JOURNAL_VERSION "1"
#define TRANSACTION_LINE_LEN 512
#define TRANSACTION_FIELDS 0xffu

// TRANSACTION MODEL
void transaction_init(Transaction *transaction) {
    if (transaction != NULL) {
        memset(transaction, 0, sizeof(*transaction));
    }
}

const char *transaction_operation_name(TransactionOperation operation) {
    if (operation == TRANSACTION_INSTALL) {
        return "install";
    }

    if (operation == TRANSACTION_REMOVE) {
        return "remove";
    }

    return "none";
}

// JOURNAL PERSISTENCE
static CupError save_journal(const Transaction *transaction) {
    CupError err;
    FILE *file = NULL;
    char path[MAX_PATH_LEN];
    char tmp_dir[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int failed = 0;

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        system_create_temp_file(tmp_dir, "transaction", temporary,
            sizeof(temporary), &file) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (fprintf(file, "journal_version=%s\n", JOURNAL_VERSION) < 0 ||
        fprintf(file, "operation=%s\n", transaction_operation_name(transaction->operation)) < 0 ||
        fprintf(file, "component=%s\n", transaction->package.component) < 0 ||
        fprintf(file, "tool=%s\n", transaction->package.tool) < 0 ||
        fprintf(file, "host_platform=%s\n", transaction->package.host_platform) < 0 ||
        fprintf(file, "target_platform=%s\n", transaction->package.target_platform) < 0 ||
        fprintf(file, "package_version=%s\n", transaction->package.version) < 0 ||
        fprintf(file, "temporary_name=%s\n", transaction->temporary_name) < 0) {
        failed = 1;
    }

    if (!failed && system_sync_file(file) != CUP_OK) {
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
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
            system_remove_file(temporary);
            return CUP_ERR_TRANSACTION;
        }
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

CupError transaction_begin(TransactionOperation operation,
    const PackageIdentity *package, const char *temporary_path) {
    Transaction transaction;
    const char *name;
    char journal[MAX_PATH_LEN];
    int exists;

    if (package == NULL || text_is_empty(temporary_path) ||
        (operation != TRANSACTION_INSTALL && operation != TRANSACTION_REMOVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (layout_get_transaction_path(journal, sizeof(journal)) != CUP_OK ||
        system_path_exists(journal, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (exists) {
        fprintf(stderr, "Error: an interrupted cup transaction must be repaired first.\n");
        return CUP_ERR_TRANSACTION;
    }

    name = path_last_segment(temporary_path);
    if (!path_is_safe_segment(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    transaction_init(&transaction);
    transaction.operation = operation;
    transaction.package = *package;

    if (text_format(transaction.temporary_name,
        sizeof(transaction.temporary_name), "%s", name) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    return save_journal(&transaction);
}

static CupError set_transaction_field(Transaction *transaction,
    const char *key, const char *value, unsigned *seen) {
    unsigned bit;
    char *destination = NULL;
    size_t destination_size = 0;

    if (strcmp(key, "journal_version") == 0) {
        bit = 1u << 0;
        if (strcmp(value, JOURNAL_VERSION) != 0) {
            return CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        bit = 1u << 1;

        if (strcmp(value, "install") == 0) {
            transaction->operation = TRANSACTION_INSTALL;
        } else if (strcmp(value, "remove") == 0) {
            transaction->operation = TRANSACTION_REMOVE;
        } else {
            return CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "component") == 0) {
        bit = 1u << 2;
        destination = transaction->package.component;
        destination_size = sizeof(transaction->package.component);
    } else if (strcmp(key, "tool") == 0) {
        bit = 1u << 3;
        destination = transaction->package.tool;
        destination_size = sizeof(transaction->package.tool);
    } else if (strcmp(key, "host_platform") == 0) {
        bit = 1u << 4;
        destination = transaction->package.host_platform;
        destination_size = sizeof(transaction->package.host_platform);
    } else if (strcmp(key, "target_platform") == 0) {
        bit = 1u << 5;
        destination = transaction->package.target_platform;
        destination_size = sizeof(transaction->package.target_platform);
    } else if (strcmp(key, "package_version") == 0) {
        bit = 1u << 6;
        destination = transaction->package.version;
        destination_size = sizeof(transaction->package.version);
    } else if (strcmp(key, "temporary_name") == 0) {
        bit = 1u << 7;
        destination = transaction->temporary_name;
        destination_size = sizeof(transaction->temporary_name);
    } else {
        return CUP_ERR_TRANSACTION;
    }

    if ((*seen & bit) != 0) {
        return CUP_ERR_TRANSACTION;
    }

    *seen |= bit;

    if (destination != NULL &&
        text_format(destination, destination_size, "%s", value) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    return CUP_OK;
}

CupError transaction_load(Transaction *transaction, TransactionFileStatus *status) {
    FILE *file;
    CupError err;
    char path[MAX_PATH_LEN];
    char line[TRANSACTION_LINE_LEN];
    size_t line_number = 0;
    unsigned seen = 0;
    PackageIdentity validated;

    if (transaction == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    transaction_init(transaction);
    *status = TRANSACTION_FILE_MISSING;

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

        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK ||
            set_transaction_field(transaction, key, value, &seen) != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }

    if (fclose(file) != 0 || seen != TRANSACTION_FIELDS ||
        !path_is_safe_segment(transaction->temporary_name) ||
        package_identity_init(&validated, transaction->package.component,
            transaction->package.tool, transaction->package.host_platform,
            transaction->package.target_platform, transaction->package.version) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    transaction->package = validated;
    *status = TRANSACTION_FILE_LOADED;
    return CUP_OK;
}

CupError transaction_get_tmp_path(const Transaction *transaction, char *buffer, size_t size) {
    CupError err;
    char tmp_dir[MAX_PATH_LEN];

    if (transaction == NULL || buffer == NULL || size == 0 ||
        !path_is_safe_segment(transaction->temporary_name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir));
    if (err != CUP_OK) {
        return err;
    }

    return path_join(buffer, size, tmp_dir, transaction->temporary_name);
}

CupError transaction_clear(void) {
    char path[MAX_PATH_LEN];

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    {
        int exists;

        if (system_path_exists(path, &exists) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        if (!exists) {
            return CUP_OK;
        }
    }

    if (system_remove_file(path) != CUP_OK ||
        system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    return CUP_OK;
}
