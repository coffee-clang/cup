/*
 * Persists and validates package-operation journals and recovers interrupted install and remove
 * operations from their commit points.
 */

#include "package_transaction.h"

#include "package_selector.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define JOURNAL_FORMAT "1"
#define TRANSACTION_LINE_LEN 512
#define FIELD_VERSION (1u << 0)
#define FIELD_OPERATION (1u << 1)
#define FIELD_COMPONENT (1u << 2)
#define FIELD_TOOL (1u << 3)
#define FIELD_HOST (1u << 4)
#define FIELD_TARGET (1u << 5)
#define FIELD_PACKAGE_VERSION (1u << 6)
#define FIELD_TEMPORARY_NAME (1u << 7)
#define COMMON_FIELDS (FIELD_VERSION | FIELD_OPERATION | FIELD_TEMPORARY_NAME)
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

static int temporary_name_matches_package_transaction(const PackageTransaction *transaction) {
    char prefix[MAX_PATH_LEN];
    size_t prefix_length;

    if (transaction == NULL || !package_operation_is_valid(transaction->operation) ||
        !path_is_safe_segment(transaction->temporary_name)) {
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
static CupError save_package_journal(const PackageTransaction *transaction) {
    CupError err;
    FILE *file = NULL;
    char path[MAX_PATH_LEN];
    char tmp_dir[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int failed = 0;

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        layout_get_staging_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK ||
        system_create_temp_file(tmp_dir, "transaction", temporary, sizeof(temporary), &file) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    if (fprintf(file, "format=%s\n", JOURNAL_FORMAT) < 0 ||
        fprintf(file, "operation=%s\n", package_operation_name(transaction->operation)) < 0) {
        failed = 1;
    }

    if (!failed && package_operation_is_valid(transaction->operation) &&
        (fprintf(file, "component=%s\n", transaction->package.component) < 0 ||
         fprintf(file, "tool=%s\n", transaction->package.tool) < 0 ||
         fprintf(file, "host_platform=%s\n", transaction->package.host_platform) < 0 ||
         fprintf(file, "target_platform=%s\n", transaction->package.target_platform) < 0 ||
         fprintf(file, "package_version=%s\n", transaction->package.version) < 0)) {
        failed = 1;
    }

    if (!failed && fprintf(file, "temporary_name=%s\n", transaction->temporary_name) < 0) {
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
    if (err == CUP_OK) {
        return CUP_OK;
    }
    if (commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
        return CUP_ERR_TRANSACTION;
    }
    return CUP_ERR_COMMIT;
}

static CupError begin_package_transaction(PackageOperation operation,
                                          const PackageIdentity *package,
                                          const char *temporary_path) {
    PackageTransaction transaction;
    const char *name;
    char journal[MAX_PATH_LEN];
    int exists;

    if (text_is_empty(temporary_path) || !package_operation_is_valid(operation) ||
        package == NULL) {
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

    package_transaction_init(&transaction);
    transaction.operation = operation;
    if (package != NULL) {
        transaction.package = *package;
    }

    if (text_copy(transaction.temporary_name, sizeof(transaction.temporary_name), name) != CUP_OK ||
        !temporary_name_matches_package_transaction(&transaction)) {
        return CUP_ERR_TRANSACTION;
    }

    return save_package_journal(&transaction);
}

CupError package_transaction_begin(PackageOperation operation,
                                   const PackageIdentity *package,
                                   const char *temporary_path) {
    return begin_package_transaction(operation, package, temporary_path);
}

static CupError set_package_transaction_field(PackageTransaction *transaction,
                                              const char *key,
                                              const char *value,
                                              unsigned *seen) {
    unsigned bit;
    char *destination = NULL;
    size_t destination_size = 0;

    if (strcmp(key, "format") == 0) {
        bit = FIELD_VERSION;
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

CupError package_transaction_load(PackageTransaction *transaction,
                                  PackageTransactionStatus *status) {
    PackageTransaction candidate;
    FILE *file;
    CupError err;
    char path[MAX_PATH_LEN];
    char line[TRANSACTION_LINE_LEN];
    size_t line_number = 0;
    unsigned seen = 0;
    unsigned expected;
    PackageIdentity validated;

    if (transaction == NULL || status == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    package_transaction_init(transaction);
    package_transaction_init(&candidate);
    *status = PACKAGE_TRANSACTION_MISSING;
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
        if (err == CUP_OK) {
            err = set_package_transaction_field(&candidate, key, value, &seen);
        }
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }

    if (fclose(file) != 0 || !package_operation_is_valid(candidate.operation)) {
        return CUP_ERR_TRANSACTION;
    }

    expected = PACKAGE_FIELDS;
    if (seen != expected) {
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

    if (!temporary_name_matches_package_transaction(&candidate)) {
        return CUP_ERR_TRANSACTION;
    }

    *transaction = candidate;
    *status = PACKAGE_TRANSACTION_LOADED;
    return CUP_OK;
}

CupError package_transaction_get_staging_path(const PackageTransaction *transaction,
                                              char *buffer,
                                              size_t size) {
    CupError err;
    char tmp_dir[MAX_PATH_LEN];

    if (transaction == NULL || buffer == NULL || size == 0 ||
        !temporary_name_matches_package_transaction(transaction)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = layout_get_staging_dir(tmp_dir, sizeof(tmp_dir));
    if (err != CUP_OK) {
        return err;
    }
    return path_join(buffer, size, tmp_dir, transaction->temporary_name);
}

CupError package_transaction_clear(void) {
    char path[MAX_PATH_LEN];
    int exists;

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (!exists) {
        return CUP_OK;
    }

    if (system_remove_file(path) != CUP_OK || system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

/* Reconcile canonical and staged package paths only when valid state determines one
 * unambiguous result. */
static CupError inspect_package_path(const char *path, int *exists) {
    return system_path_exists(path, exists) == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static int package_path_is_valid(const char *path, int exists, const PackageIdentity *package) {
    SystemPathKind kind;

    return exists && system_get_path_kind(path, &kind) == CUP_OK && kind == SYSTEM_PATH_DIRECTORY &&
           package_validate(path, package) == CUP_OK;
}

static CupError move_staged_package(const PackageIdentity *package,
                                    const char *staged_path,
                                    const char *install_path) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    err = layout_ensure_package_parent(package);
    if (err != CUP_OK) {
        return CUP_ERR_TRANSACTION;
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
    return filesystem_remove_tree(path) == CUP_OK ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError preserve_invalid_install(const char *install_path) {
    CupError err;
    char backup_path[MAX_PATH_LEN];

    err = filesystem_backup_invalid(install_path, backup_path, sizeof(backup_path));
    if (err != CUP_OK) {
        return err == CUP_ERR_COMMIT ? err : CUP_ERR_TRANSACTION;
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
    char selector[MAX_SELECTOR_LEN];
    char install_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_installed;
    int install_exists;
    int install_valid;
    int staged_exists;
    int staged_valid;

    if (transaction == NULL || !package_operation_is_valid(transaction->operation)) {
        return CUP_ERR_TRANSACTION;
    }
    if (state == NULL) {
        return CUP_ERR_TRANSACTION;
    }

    if (package_selector_format_parts(
            selector, sizeof(selector), transaction->package.tool, transaction->package.version) !=
            CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path), &transaction->package) !=
            CUP_OK ||
        package_transaction_get_staging_path(transaction, staged_path, sizeof(staged_path)) !=
            CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    is_installed = state_find_installed(state, &transaction->package) != -1;

    err = inspect_package_path(install_path, &install_exists);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_package_path(staged_path, &staged_exists);
    if (err != CUP_OK) {
        return err;
    }

    install_valid = 0;
    staged_valid = 0;
    if (is_installed) {
        install_valid = package_path_is_valid(install_path, install_exists, &transaction->package);
        staged_valid = package_path_is_valid(staged_path, staged_exists, &transaction->package);
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

    err = package_transaction_clear();
    if (err == CUP_OK) {
        printf("Recovered interrupted %s transaction for %s@%s.\n",
               package_operation_name(transaction->operation),
               transaction->package.tool,
               transaction->package.version);
    }
    return err;
}
