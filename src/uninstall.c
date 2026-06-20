#include "commands.h"

#include "layout.h"
#include "system.h"
#include "transaction.h"
#include "util.h"

#include <stdio.h>

#if defined(_WIN32)
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup-windows.ps1"
#else
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup.sh"
#endif

// UNINSTALL COMMAND
CupError handle_uninstall(void) {
    Transaction transaction;
    TransactionFileStatus transaction_status;
    SystemLock lock = {0, 0};
    CupError err;
    char root[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char script[MAX_PATH_LEN];
    char answer[16];
    int is_directory;
    int is_file;

    transaction_init(&transaction);

    err = layout_get_root(root, sizeof(root));
    if (err != CUP_OK) {
        return err;
    }

    err = system_is_directory(root, &is_directory);
    if (err != CUP_OK || !is_directory) {
        fprintf(stderr, "Error: cup has no managed data at '%s'.\n", root);
        return CUP_ERR_FILESYSTEM;
    }

    if (layout_get_lock_path(lock_path, sizeof(lock_path)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_lock_acquire(&lock, lock_path, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    err = transaction_load(&transaction, &transaction_status);
    if (err != CUP_OK || transaction_status == TRANSACTION_FILE_LOADED) {
        fprintf(stderr, "Error: an interrupted transaction must be repaired before uninstalling cup.\n");
        err = CUP_ERR_TRANSACTION;
        goto done;
    }

    err = layout_get_uninstall_path(script, sizeof(script));
    if (err != CUP_OK) {
        goto done;
    }

    err = system_is_regular_file(script, &is_file);
    if (err != CUP_OK) {
        goto done;
    }

    if (!is_file) {
        err = system_is_regular_file(DEVELOPMENT_UNINSTALL_PATH, &is_file);
        if (err != CUP_OK || !is_file) {
            fprintf(stderr, "Error: no installed or development uninstall script was found.\n");
            err = CUP_ERR_FILESYSTEM;
            goto done;
        }

        err = checked_snprintf(script, sizeof(script), "%s",
            DEVELOPMENT_UNINSTALL_PATH);
        if (err != CUP_OK) {
            goto done;
        }
    }

    printf("This will remove cup and all cup-managed data from:\n  %s\n\n", root);
    printf("The PATH entry will not be removed.\nContinue? [y/N] ");

    if (fgets(answer, sizeof(answer), stdin) == NULL ||
        (answer[0] != 'y' && answer[0] != 'Y')) {
        printf("Uninstall cancelled.\n");
        err = CUP_OK;
        goto done;
    }

    err = system_start_uninstall(root, script);
    if (err == CUP_OK) {
        printf("Uninstall started. The PATH entry was not removed.\n");
    }

done:
    system_lock_release(&lock);
    return err;
}
