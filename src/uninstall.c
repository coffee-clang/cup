#include "commands.h"

#include "layout.h"
#include "system.h"
#include "transaction.h"
#include "text.h"

#include <stdio.h>

static CupError find_uninstall_script(char *script_path, size_t path_size) {
    CupError err;
    int is_regular_file;

    err = layout_get_uninstall_path(script_path, path_size);
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_regular_file(script_path, &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }
    if (is_regular_file) {
        return CUP_OK;
    }

    err = system_is_regular_file(CUP_DEVELOPMENT_UNINSTALL_PATH,
        &is_regular_file);
    if (err != CUP_OK) {
        return err;
    }
    if (!is_regular_file) {
        fprintf(stderr,
            "Error: no installed or development uninstall script was found.\n");
        return CUP_ERR_FILESYSTEM;
    }

    return text_format(script_path, path_size, "%s",
        CUP_DEVELOPMENT_UNINSTALL_PATH);
}

static int confirm_uninstall(const char *root_path) {
    char answer[16];

    printf("This will remove cup and all cup-managed data from:\n  %s\n\n",
        root_path);
    printf("The PATH entry will not be removed.\nContinue? [y/N] ");

    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        return 0;
    }

    return answer[0] == 'y' || answer[0] == 'Y';
}

CupError command_uninstall(void) {
    Transaction transaction;
    TransactionFileStatus transaction_status;
    SystemLock lock = {0, 0};
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char script_path[MAX_PATH_LEN];
    int root_is_directory;

    transaction_init(&transaction);

    err = layout_get_root(root_path, sizeof(root_path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_directory(root_path, &root_is_directory);
    if (err != CUP_OK) {
        return err;
    }
    if (!root_is_directory) {
        fprintf(stderr, "Error: cup has no managed data at '%s'.\n", root_path);
        return CUP_ERR_FILESYSTEM;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr,
                "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    err = transaction_load(&transaction, &transaction_status);
    if (err != CUP_OK || transaction_status == TRANSACTION_FILE_LOADED) {
        fprintf(stderr, "Error: an interrupted transaction must be repaired "
            "before uninstalling cup.\n");
        err = CUP_ERR_TRANSACTION;
        goto done;
    }

    err = find_uninstall_script(script_path, sizeof(script_path));
    if (err != CUP_OK) {
        goto done;
    }

    if (!confirm_uninstall(root_path)) {
        printf("Uninstall cancelled.\n");
        err = CUP_OK;
        goto done;
    }

    err = system_start_uninstall(root_path, script_path);
    if (err == CUP_OK) {
        printf("Uninstall started. The PATH entry was not removed.\n");
    }

done:
    system_lock_release(&lock);
    return err;
}
