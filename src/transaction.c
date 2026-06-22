#include "transaction.h"

#include "bootstrap.h"
#include "entry.h"
#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "system.h"
#include "text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define JOURNAL_VERSION "2"
#define LEGACY_JOURNAL_VERSION "1"
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
#define PACKAGE_FIELDS (COMMON_FIELDS | FIELD_COMPONENT | FIELD_TOOL | \
    FIELD_HOST | FIELD_TARGET | FIELD_PACKAGE_VERSION)

typedef struct {
    const char *backup_name;
    const char *destination;
    int executable;
    int read_only;
} SelfUpdateAsset;

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
    if (operation == TRANSACTION_SELF_UPDATE) {
        return "self-update";
    }
    return "none";
}

static int transaction_operation_is_valid(TransactionOperation operation) {
    return operation == TRANSACTION_INSTALL ||
        operation == TRANSACTION_REMOVE ||
        operation == TRANSACTION_SELF_UPDATE;
}

static int transaction_uses_package(TransactionOperation operation) {
    return operation == TRANSACTION_INSTALL ||
        operation == TRANSACTION_REMOVE;
}

static int temporary_name_matches_transaction(const Transaction *transaction) {
    char prefix[MAX_PATH_LEN];
    size_t prefix_length;

    if (transaction == NULL ||
        !transaction_operation_is_valid(transaction->operation) ||
        !path_is_safe_segment(transaction->temporary_name)) {
        return 0;
    }

    if (transaction->operation == TRANSACTION_SELF_UPDATE) {
        return strncmp(transaction->temporary_name, "self-update-", 12) == 0 &&
            transaction->temporary_name[12] != '\0';
    }

    if (layout_build_tmp_prefix(prefix, sizeof(prefix),
            transaction_operation_name(transaction->operation),
            &transaction->package) != CUP_OK) {
        return 0;
    }

    prefix_length = strlen(prefix);
    return strncmp(transaction->temporary_name, prefix, prefix_length) == 0 &&
        transaction->temporary_name[prefix_length] == '-' &&
        transaction->temporary_name[prefix_length + 1] != '\0';
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
        fprintf(file, "operation=%s\n",
            transaction_operation_name(transaction->operation)) < 0) {
        failed = 1;
    }

    if (!failed && transaction_uses_package(transaction->operation) &&
        (fprintf(file, "component=%s\n", transaction->package.component) < 0 ||
         fprintf(file, "tool=%s\n", transaction->package.tool) < 0 ||
         fprintf(file, "host_platform=%s\n",
             transaction->package.host_platform) < 0 ||
         fprintf(file, "target_platform=%s\n",
             transaction->package.target_platform) < 0 ||
         fprintf(file, "package_version=%s\n",
             transaction->package.version) < 0)) {
        failed = 1;
    }

    if (!failed && fprintf(file, "temporary_name=%s\n",
            transaction->temporary_name) < 0) {
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

static CupError begin_transaction(TransactionOperation operation,
    const PackageIdentity *package, const char *temporary_path) {
    Transaction transaction;
    const char *name;
    char journal[MAX_PATH_LEN];
    int exists;

    if (text_is_empty(temporary_path) ||
        !transaction_operation_is_valid(operation) ||
        (transaction_uses_package(operation) && package == NULL)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (layout_get_transaction_path(journal, sizeof(journal)) != CUP_OK ||
        system_path_exists(journal, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (exists) {
        fprintf(stderr,
            "Error: an interrupted cup transaction must be repaired first.\n");
        return CUP_ERR_TRANSACTION;
    }

    name = path_last_segment(temporary_path);
    if (!path_is_safe_segment(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    transaction_init(&transaction);
    transaction.operation = operation;
    if (package != NULL) {
        transaction.package = *package;
    }

    if (text_copy(transaction.temporary_name,
            sizeof(transaction.temporary_name), name) != CUP_OK ||
        !temporary_name_matches_transaction(&transaction)) {
        return CUP_ERR_TRANSACTION;
    }

    return save_journal(&transaction);
}

CupError transaction_begin(TransactionOperation operation,
    const PackageIdentity *package, const char *temporary_path) {
    if (!transaction_uses_package(operation)) {
        return CUP_ERR_INVALID_INPUT;
    }
    return begin_transaction(operation, package, temporary_path);
}

CupError transaction_begin_self_update(const char *temporary_path) {
    return begin_transaction(TRANSACTION_SELF_UPDATE, NULL, temporary_path);
}

static CupError set_transaction_field(Transaction *transaction,
    const char *key, const char *value, unsigned *seen) {
    unsigned bit;
    char *destination = NULL;
    size_t destination_size = 0;

    if (strcmp(key, "journal_version") == 0) {
        bit = FIELD_VERSION;
        if (strcmp(value, JOURNAL_VERSION) != 0 &&
            strcmp(value, LEGACY_JOURNAL_VERSION) != 0) {
            return CUP_ERR_TRANSACTION;
        }
    } else if (strcmp(key, "operation") == 0) {
        bit = FIELD_OPERATION;
        if (strcmp(value, "install") == 0) {
            transaction->operation = TRANSACTION_INSTALL;
        } else if (strcmp(value, "remove") == 0) {
            transaction->operation = TRANSACTION_REMOVE;
        } else if (strcmp(value, "self-update") == 0) {
            transaction->operation = TRANSACTION_SELF_UPDATE;
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

    if (destination != NULL &&
        text_copy(destination, destination_size, value) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

CupError transaction_load(Transaction *transaction,
    TransactionFileStatus *status) {
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

        err = text_read_line(file, line, sizeof(line),
            &has_line, &line_number);
        if (err != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
        if (!has_line) {
            break;
        }

        if (text_parse_key_value(line, key, sizeof(key),
            value, sizeof(value)) != CUP_OK ||
            set_transaction_field(transaction, key, value, &seen) != CUP_OK) {
            fclose(file);
            return CUP_ERR_TRANSACTION;
        }
    }

    if (fclose(file) != 0 ||
        !transaction_operation_is_valid(transaction->operation)) {
        return CUP_ERR_TRANSACTION;
    }

    expected = transaction_uses_package(transaction->operation)
        ? PACKAGE_FIELDS : COMMON_FIELDS;
    if (seen != expected) {
        return CUP_ERR_TRANSACTION;
    }

    if (transaction_uses_package(transaction->operation)) {
        if (package_identity_init(&validated, transaction->package.component,
                transaction->package.tool,
                transaction->package.host_platform,
                transaction->package.target_platform,
                transaction->package.version) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
        transaction->package = validated;
    }

    if (!temporary_name_matches_transaction(transaction)) {
        return CUP_ERR_TRANSACTION;
    }

    *status = TRANSACTION_FILE_LOADED;
    return CUP_OK;
}

CupError transaction_get_tmp_path(const Transaction *transaction,
    char *buffer, size_t size) {
    CupError err;
    char tmp_dir[MAX_PATH_LEN];

    if (transaction == NULL || buffer == NULL || size == 0 ||
        !temporary_name_matches_transaction(transaction)) {
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
    int exists;

    if (layout_get_transaction_path(path, sizeof(path)) != CUP_OK ||
        system_path_exists(path, &exists) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (!exists) {
        return CUP_OK;
    }

    if (system_remove_file(path) != CUP_OK ||
        system_sync_parent_directory(path) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

// SELF-UPDATE RECOVERY
static CupError set_asset_permissions(const SelfUpdateAsset *asset) {
    if (asset->executable && system_set_executable(asset->destination, 1) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (asset->read_only && system_set_read_only(asset->destination, 1) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    return CUP_OK;
}

static CupError restore_self_update_asset(const char *staging,
    const SelfUpdateAsset *asset) {
    char backup[MAX_PATH_LEN];
    SystemPathKind kind;
    SystemCommitState state = SYSTEM_COMMIT_NOT_APPLIED;

    if (path_join(backup, sizeof(backup), staging,
            asset->backup_name) != CUP_OK ||
        system_get_path_kind(backup, &kind) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        return CUP_OK;
    }
    if (kind != SYSTEM_PATH_REGULAR_FILE) {
        return CUP_ERR_TRANSACTION;
    }

    if (system_get_path_kind(asset->destination, &kind) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }
    if (kind != SYSTEM_PATH_MISSING) {
        if (kind != SYSTEM_PATH_REGULAR_FILE ||
            system_set_read_only(asset->destination, 0) != CUP_OK ||
            system_remove_file(asset->destination) != CUP_OK) {
            return CUP_ERR_TRANSACTION;
        }
    }

    if (system_move_path(backup, asset->destination, &state) != CUP_OK ||
        set_asset_permissions(asset) != CUP_OK) {
        return CUP_ERR_ROLLBACK;
    }
    return CUP_OK;
}

static CupError recover_self_update(const Transaction *transaction) {
    BootstrapInspection inspection;
    CupError err;
    char staging[MAX_PATH_LEN];
    char marker[MAX_PATH_LEN];
    char binary[MAX_PATH_LEN];
    char uninstall[MAX_PATH_LEN];
    char checksums[MAX_PATH_LEN];
    SystemPathKind marker_kind;
    SelfUpdateAsset assets[3];
    size_t i;

    if (transaction_get_tmp_path(transaction, staging,
            sizeof(staging)) != CUP_OK ||
        path_join(marker, sizeof(marker), staging,
            CUP_SELF_UPDATE_COMMITTED) != CUP_OK ||
        layout_get_binary_path(binary, sizeof(binary)) != CUP_OK ||
        layout_get_uninstall_path(uninstall, sizeof(uninstall)) != CUP_OK ||
        layout_get_platform_checksums_path(checksums,
            sizeof(checksums)) != CUP_OK ||
        system_get_path_kind(marker, &marker_kind) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    assets[0] = (SelfUpdateAsset){CUP_SELF_UPDATE_BINARY_OLD, binary, 1, 0};
    assets[1] = (SelfUpdateAsset){CUP_SELF_UPDATE_UNINSTALL_OLD, uninstall, 1, 1};
    assets[2] = (SelfUpdateAsset){CUP_SELF_UPDATE_CHECKSUMS_OLD, checksums, 0, 1};

    if (marker_kind == SYSTEM_PATH_REGULAR_FILE) {
        err = bootstrap_inspect(&inspection);
        if (err == CUP_OK && bootstrap_installed_is_valid(&inspection)) {
            err = transaction_clear();
            if (err == CUP_OK) {
                err = filesystem_remove_tree(staging);
            }
            if (err == CUP_OK) {
                printf("Completed interrupted self-update transaction.\n");
            }
            return err;
        }
    } else if (marker_kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_TRANSACTION;
    }

    for (i = 0; i < sizeof(assets) / sizeof(assets[0]); ++i) {
        err = restore_self_update_asset(staging, &assets[i]);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = bootstrap_inspect(&inspection);
    if (err != CUP_OK || !bootstrap_installed_is_valid(&inspection)) {
        return CUP_ERR_TRANSACTION;
    }

    err = transaction_clear();
    if (err == CUP_OK) {
        err = filesystem_remove_tree(staging);
    }
    if (err == CUP_OK) {
        printf("Rolled back interrupted self-update transaction.\n");
    }
    return err;
}

// PACKAGE TRANSACTION RECOVERY
static CupError inspect_package_path(const char *path, int *exists) {
    return system_path_exists(path, exists) == CUP_OK
        ? CUP_OK : CUP_ERR_TRANSACTION;
}

static int package_path_is_valid(const char *path, int exists,
    const PackageIdentity *package) {
    SystemPathKind kind;

    return exists &&
        system_get_path_kind(path, &kind) == CUP_OK &&
        kind == SYSTEM_PATH_DIRECTORY &&
        package_validate(path, package) == CUP_OK;
}

static CupError move_staged_package(const PackageIdentity *package,
    const char *staged_path, const char *install_path) {
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
    return commit_state == SYSTEM_COMMIT_APPLIED
        ? CUP_ERR_COMMIT : CUP_ERR_TRANSACTION;
}

static CupError remove_transaction_path(const char *path, int exists) {
    if (!exists) {
        return CUP_OK;
    }
    return filesystem_remove_tree(path) == CUP_OK
        ? CUP_OK : CUP_ERR_TRANSACTION;
}

static CupError recover_installed_package(const Transaction *transaction,
    const char *install_path, int install_exists, int install_valid,
    const char *staged_path, int staged_exists, int staged_valid) {
    if (transaction->operation == TRANSACTION_REMOVE) {
        if (install_exists) {
            return remove_transaction_path(staged_path, staged_exists);
        }
        if (!staged_exists) {
            return CUP_ERR_TRANSACTION;
        }
        return move_staged_package(&transaction->package,
            staged_path, install_path);
    }

    if (install_valid) {
        return remove_transaction_path(staged_path, staged_exists);
    }
    if (install_exists || !staged_valid) {
        return CUP_ERR_TRANSACTION;
    }
    return move_staged_package(&transaction->package,
        staged_path, install_path);
}

static CupError recover_absent_package(const char *install_path,
    int install_exists, const char *staged_path, int staged_exists) {
    CupError err;

    err = remove_transaction_path(install_path, install_exists);
    if (err != CUP_OK) {
        return err;
    }
    return remove_transaction_path(staged_path, staged_exists);
}

CupError transaction_recover(const Transaction *transaction, CupState *state) {
    CupError err;
    char entry[MAX_ENTRY_LEN];
    char install_path[MAX_PATH_LEN];
    char staged_path[MAX_PATH_LEN];
    int is_installed;
    int install_exists;
    int install_valid;
    int staged_exists;
    int staged_valid;

    if (transaction == NULL ||
        !transaction_operation_is_valid(transaction->operation)) {
        return CUP_ERR_TRANSACTION;
    }
    if (transaction->operation == TRANSACTION_SELF_UPDATE) {
        return recover_self_update(transaction);
    }
    if (state == NULL) {
        return CUP_ERR_TRANSACTION;
    }

    if (entry_build(entry, sizeof(entry), transaction->package.tool,
        transaction->package.version) != CUP_OK ||
        layout_build_install_path(install_path, sizeof(install_path),
            &transaction->package) != CUP_OK ||
        transaction_get_tmp_path(transaction, staged_path,
            sizeof(staged_path)) != CUP_OK) {
        return CUP_ERR_TRANSACTION;
    }

    is_installed = state_find_installed(state,
        transaction->package.component,
        transaction->package.host_platform,
        transaction->package.target_platform, entry) != -1;

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
    if (is_installed && transaction->operation == TRANSACTION_INSTALL) {
        install_valid = package_path_is_valid(install_path, install_exists,
            &transaction->package);
        staged_valid = package_path_is_valid(staged_path, staged_exists,
            &transaction->package);
    }

    if (is_installed) {
        err = recover_installed_package(transaction,
            install_path, install_exists, install_valid,
            staged_path, staged_exists, staged_valid);
    } else {
        err = recover_absent_package(install_path, install_exists,
            staged_path, staged_exists);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = transaction_clear();
    if (err == CUP_OK) {
        printf("Recovered interrupted %s transaction for %s@%s.\n",
            transaction_operation_name(transaction->operation),
            transaction->package.tool, transaction->package.version);
    }
    return err;
}
