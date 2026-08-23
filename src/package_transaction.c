/*
 * Persists and validates package-operation journals and recovers interrupted install, update and
 * remove operations from their commit points.
 */

#include "package_transaction.h"

#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "runtime_journal.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define JOURNAL_FORMAT "1"
#define FIELD_FORMAT (1u << 0)
#define FIELD_OPERATION (1u << 1)
#define FIELD_COMPONENT (1u << 2)
#define FIELD_TOOL (1u << 3)
#define FIELD_HOST (1u << 4)
#define FIELD_TARGET (1u << 5)
#define FIELD_PACKAGE_VERSION (1u << 6)
#define FIELD_TEMPORARY_NAME (1u << 7)
#define COMMON_FIELDS (FIELD_FORMAT | FIELD_OPERATION | FIELD_TEMPORARY_NAME)
#define PACKAGE_FIELDS \
    (COMMON_FIELDS | FIELD_COMPONENT | FIELD_TOOL | FIELD_HOST | FIELD_TARGET | \
     FIELD_PACKAGE_VERSION)

/* Package journal schema and operation names. state.txt remains the commit point for
 * install/remove. */
void package_transaction_init(PackageTransaction *transaction) {
    if (transaction != NULL) {
        memset(transaction, 0, sizeof(*transaction));
    }
}

const char *package_operation_name(PackageOperation operation) {
    if (operation == PACKAGE_OPERATION_INSTALL) {
        return "install";
    }
    if (operation == PACKAGE_OPERATION_REMOVE) {
        return "remove";
    }
    if (operation == PACKAGE_OPERATION_UPDATE) {
        return "update";
    }
    return "none";
}

static int package_operation_is_valid(PackageOperation operation) {
    return operation == PACKAGE_OPERATION_INSTALL || operation == PACKAGE_OPERATION_REMOVE ||
           operation == PACKAGE_OPERATION_UPDATE;
}

static int transaction_temporary_name_is_valid(const PackageTransaction *transaction) {
    char prefix[MAX_PATH_LEN];
    size_t prefix_length;

    if (transaction == NULL || !package_operation_is_valid(transaction->operation) ||
        !path_is_safe_segment(transaction->temporary_name) ||
        strlen(transaction->temporary_name) >= MAX_METADATA_VALUE_LEN) {
        return 0;
    }

    if (layout_build_staging_prefix(prefix,
                                    sizeof(prefix),
                                    package_operation_name(transaction->operation),
                                    &transaction->package) != CUP_OK) {
        return 0;
    }

    prefix_length = strlen(prefix);
    return strncmp(transaction->temporary_name, prefix, prefix_length) == 0 &&
           transaction->temporary_name[prefix_length] == '-' &&
           transaction->temporary_name[prefix_length + 1] != '\0';
}

/* Write and parse transaction.txt as a strict all-or-nothing format=1 record set. */
static CupError write_package_journal(FILE *file, const void *value) {
    const PackageTransaction *transaction = value;

    if (transaction == NULL ||
        fprintf(file, "format=%s\n", JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=%s\n", package_operation_name(transaction->operation)) < 0 ||
        fprintf(file, "component=%s\n", transaction->package.component) < 0 ||
        fprintf(file, "tool=%s\n", transaction->package.tool) < 0 ||
        fprintf(file, "host_platform=%s\n", transaction->package.host_platform) < 0 ||
        fprintf(file, "target_platform=%s\n", transaction->package.target_platform) < 0 ||
        fprintf(file, "package_version=%s\n", transaction->package.version) < 0 ||
        fprintf(file, "temporary_name=%s\n", transaction->temporary_name) < 0) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError save_package_journal(PackageTransaction *transaction) {
    char staging_dir[MAX_PATH_LEN];
    SystemPathIdentity published_identity;
    const SystemPathIdentity *expected_identity;
    CupError err;

    if (transaction == NULL || !package_operation_is_valid(transaction->operation) ||
        !transaction_temporary_name_is_valid(transaction) ||
        layout_get_staging_dir(staging_dir, sizeof(staging_dir)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    expected_identity = transaction->file_identity.valid
                            ? &transaction->file_identity
                            : NULL;
    err = runtime_journal_publish(staging_dir,
                                  "transaction",
                                  expected_identity,
                                  write_package_journal,
                                  transaction,
                                  &published_identity);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && published_identity.valid) {
        transaction->file_identity = published_identity;
    }
    return err;
}

CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path,
                                   PackageTransaction *created) {
    PackageTransaction transaction;
    CupError err;
    const char *name;

    if (created == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    package_transaction_init(created);
    if (text_is_empty(temporary_path) || !package_operation_is_valid(operation) ||
        package == NULL || package_identity_validate(package, NULL) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    name = path_last_segment(temporary_path);
    if (!path_is_safe_segment(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    package_transaction_init(&transaction);
    transaction.operation = operation;
    transaction.package = *package;
    if (text_copy(transaction.temporary_name, sizeof(transaction.temporary_name), name) != CUP_OK ||
        !transaction_temporary_name_is_valid(&transaction)) {
        return CUP_ERR_TRANSACTION;
    }
    err = save_package_journal(&transaction);
    if ((err == CUP_OK || err == CUP_ERR_COMMIT) && transaction.file_identity.valid) {
        *created = transaction;
    }
    return err;
}

static CupError set_package_transaction_field(PackageTransaction *transaction,
                                              const char *key,
                                              const char *value,
                                              unsigned *seen) {
    unsigned bit;
    char *destination = NULL;
    size_t destination_size = 0;

    if (strcmp(key, "format") == 0) {
        bit = FIELD_FORMAT;
        if (strcmp(value, JOURNAL_FORMAT) != 0) {
            return CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        bit = FIELD_OPERATION;
        if (strcmp(value, "install") == 0) {
            transaction->operation = PACKAGE_OPERATION_INSTALL;
        } else if (strcmp(value, "remove") == 0) {
            transaction->operation = PACKAGE_OPERATION_REMOVE;
        } else if (strcmp(value, "update") == 0) {
            transaction->operation = PACKAGE_OPERATION_UPDATE;
        } else {
            return CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "component") == 0) {
        bit = FIELD_COMPONENT;
        destination = transaction->package.component;
        destination_size = sizeof(transaction->package.component);
    } else if (strcmp(key, "tool") == 0) {
        bit = FIELD_TOOL;
        destination = transaction->package.tool;
        destination_size = sizeof(transaction->package.tool);
    } else if (strcmp(key, "host_platform") == 0) {
        bit = FIELD_HOST;
        destination = transaction->package.host_platform;
        destination_size = sizeof(transaction->package.host_platform);
    } else if (strcmp(key, "target_platform") == 0) {
        bit = FIELD_TARGET;
        destination = transaction->package.target_platform;
        destination_size = sizeof(transaction->package.target_platform);
    } else if (strcmp(key, "package_version") == 0) {
        bit = FIELD_PACKAGE_VERSION;
        destination = transaction->package.version;
        destination_size = sizeof(transaction->package.version);
    } else if (strcmp(key, "temporary_name") == 0) {
        bit = FIELD_TEMPORARY_NAME;
        destination = transaction->temporary_name;
        destination_size = sizeof(transaction->temporary_name);
    } else {
        return CUP_ERR_TRANSACTION;
    }

    if ((*seen & bit) != 0) {
        return CUP_ERR_TRANSACTION;
    }
    *seen |= bit;

    if (destination != NULL && text_copy(destination, destination_size, value) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static const char *const package_journal_keys[] = {
    "format", "operation", "component", "tool",
    "host_platform", "target_platform", "package_version", "temporary_name"};

typedef struct {
    PackageTransaction *candidate;
    unsigned seen;
} PackageJournalParser;

static CupError parse_package_journal_field(const char *key,
                                            const char *value,
                                            void *userdata) {
    PackageJournalParser *parser = userdata;

    if (parser == NULL) {
        return CUP_ERR_TRANSACTION;
    }
    return set_package_transaction_field(
        parser->candidate, key, value, &parser->seen);
}

CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status) {
    PackageTransaction candidate;
    PackageJournalParser parser;
    SystemPathIdentity file_identity;
    PackageIdentity validated;
    unsigned expected = PACKAGE_FIELDS;
    CupError err;
    int missing;

    if (transaction == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    package_transaction_init(transaction);
    package_transaction_init(&candidate);
    memset(&parser, 0, sizeof(parser));
    memset(&file_identity, 0, sizeof(file_identity));
    parser.candidate = &candidate;
    *status = PACKAGE_TRANSACTION_MISSING;

    err = runtime_journal_parse(package_journal_keys,
                                sizeof(package_journal_keys) /
                                    sizeof(package_journal_keys[0]),
                                parse_package_journal_field,
                                &parser,
                                &file_identity,
                                &missing);
    if (err != CUP_OK || missing) {
        return err;
    }
    if (!package_operation_is_valid(candidate.operation) ||
        parser.seen != expected) {
        return CUP_ERR_TRANSACTION;
    }

    if (package_identity_init(&validated,
                              candidate.package.component,
                              candidate.package.tool,
                              candidate.package.host_platform,
                              candidate.package.target_platform,
                              candidate.package.version) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.package = validated;

    if (!transaction_temporary_name_is_valid(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }
    candidate.file_identity = file_identity;

    *transaction = candidate;
    *status = PACKAGE_TRANSACTION_LOADED;
    return CUP_OK;
}

static CupError package_transaction_get_staging_path(const PackageTransaction *transaction,
                                                     char *buffer,
                                                     size_t size) {
    CupError err;
    char staging_dir[MAX_PATH_LEN];

    if (transaction == NULL || buffer == NULL || size == 0 ||
        !transaction_temporary_name_is_valid(transaction)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_staging_dir(staging_dir, sizeof(staging_dir));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, staging_dir, transaction->temporary_name);
}


/* Reconcile canonical and staged package paths only when valid state determines one
 * unambiguous result. */
static CupError inspect_package_path(const char *path, SystemPathKind *kind) {
    return system_get_path_kind(path, kind) == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError inspect_package_validity(const char *path,
                                         SystemPathKind kind,
                                         const PackageIdentity *package,
                                         int *valid) {
    CupError err;

    *valid = 0;
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_DIRECTORY) {
        return CUP_OK;
    }

    err = package_validate(path, package, stderr);
    if (err == CUP_OK) {
        *valid = 1;
        return CUP_OK;
    }
    return err == CUP_ERR_VALIDATION ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError move_staged_package(const PackageIdentity *package,
                                    const char *staged_path,
                                    const char *install_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    err = layout_ensure_package_parent(package);
    if (err != CUP_OK) {
        /* Directory-chain creation is retryable but can leave an earlier prefix on failure. */
        return CUP_ERR_COMMIT;
    }

    err = system_move_path(staged_path, install_path, &commit_state);
    if (err == CUP_OK) {
        return CUP_OK;
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : CUP_ERR_TRANSACTION;
}

static CupError remove_transaction_path(const char *path, int exists) {
    if (!exists) {
        return CUP_OK;
    }
    return filesystem_remove_tree(path) == CUP_OK ? CUP_OK : CUP_ERR_COMMIT;
}

static CupError preserve_invalid_install(const char *install_path) {
    CupError err;
    char backup_path[MAX_PATH_LEN];

    err = filesystem_backup_invalid(install_path, backup_path, sizeof(backup_path));
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT || err == CUP_ERR_ROLLBACK) {
            return err;
        }
        return CUP_ERR_TRANSACTION;
    }

    printf("Preserved invalid package path as '%s'.\n", backup_path);
    return CUP_OK;
}

static CupError recover_installed_package(const PackageTransaction *transaction,
                                          const char *install_path,
                                          int install_exists,
                                          int install_valid,
                                          const char *staged_path,
                                          int staged_exists,
                                          int staged_valid) {
    CupError err;

    if (install_valid) {
        return remove_transaction_path(staged_path, staged_exists);
    }
    if (!staged_valid) {
        return CUP_ERR_TRANSACTION;
    }

    if (install_exists) {
        err = preserve_invalid_install(install_path);
        if (err != CUP_OK) {
            return err;
        }
        err = move_staged_package(&transaction->package, staged_path, install_path);
        /* The durable backup is already recovery progress even if the following move was not. */
        return err == CUP_ERR_TRANSACTION ? CUP_ERR_COMMIT : err;
    }

    return move_staged_package(&transaction->package, staged_path, install_path);
}

static CupError recover_absent_package(const char *install_path,
                                       int install_exists,
                                       const char *staged_path,
                                       int staged_exists) {
    CupError err;

    err = remove_transaction_path(install_path, install_exists);
    if (err != CUP_OK) {
        return err;
    }
    return remove_transaction_path(staged_path, staged_exists);
}

CupError package_transaction_recover(const PackageTransaction *transaction, CupState *state) {
    CupError err;
    char install_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    SystemPathKind install_kind;
    SystemPathKind staged_kind;
    int is_installed;
    int install_exists;
    int install_valid;
    int staged_exists;
    int staged_valid;

    if (transaction == NULL || !package_operation_is_valid(transaction->operation) ||
        !transaction->file_identity.valid ||
        transaction->file_identity.kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }
    if (state == NULL || state->installed_count > MAX_INSTALLED) {
        return CUP_ERR_TRANSACTION;
    }

    if (package_identity_validate(&transaction->package, NULL) != CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path), &transaction->package) !=
            CUP_OK ||
        package_transaction_get_staging_path(transaction, staged_path, sizeof(staged_path)) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    is_installed = state_find_installed(state, &transaction->package) != -1;

    err = inspect_package_path(install_path, &install_kind);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_package_path(staged_path, &staged_kind);
    if (err != CUP_OK) {
        return err;
    }
    install_exists = install_kind != SYSTEM_PATH_MISSING;
    staged_exists = staged_kind != SYSTEM_PATH_MISSING;

    install_valid = 0;
    staged_valid = 0;
    if (is_installed) {
        err = inspect_package_validity(
            install_path, install_kind, &transaction->package, &install_valid);
        if (err != CUP_OK) {
            return err;
        }
        err = inspect_package_validity(
            staged_path, staged_kind, &transaction->package, &staged_valid);
        if (err != CUP_OK) {
            return err;
        }
    }

    if (is_installed) {
        err = recover_installed_package(transaction,
                                        install_path,
                                        install_exists,
                                        install_valid,
                                        staged_path,
                                        staged_exists,
                                        staged_valid);
    } else {
        err = recover_absent_package(install_path, install_exists, staged_path, staged_exists);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = runtime_journal_clear_if_identity(&transaction->file_identity);
    if (err == CUP_OK) {
        printf("Recovered interrupted %s transaction for %s@%s.\n",
               package_operation_name(transaction->operation),
               transaction->package.tool,
               transaction->package.version);
    }
    return err;
}
