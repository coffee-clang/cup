#include "commands.h"

#include "bootstrap.h"
#include "layout.h"
#include "system.h"
#include "transaction.h"
#include "text.h"

#include <stdio.h>

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

static CupError create_uninstall_marker(char *path, size_t size,
    unsigned long parent_pid) {
    FILE *file = NULL;
    CupError err;

    err = layout_get_uninstall_marker_path(path, size);
    if (err != CUP_OK) {
        return err;
    }
    err = system_create_file_exclusive(path, &file);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: an uninstall marker already exists. "
                "Run the installer again if no uninstall is active.\n");
        }
        return err;
    }

    {
        int write_failed = fprintf(file, "parent_pid=%lu\n", parent_pid) < 0;
        int sync_failed = !write_failed && system_sync_file(file) != CUP_OK;
        int close_failed = fclose(file) != 0;

        file = NULL;
        if (write_failed || sync_failed || close_failed ||
            system_sync_parent_directory(path) != CUP_OK) {
            system_remove_file(path);
            return CUP_ERR_FILESYSTEM;
        }
    }
    return CUP_OK;
}

CupError command_uninstall(void) {
    Transaction transaction;
    TransactionFileStatus transaction_status;
    SystemLock lock = {0, 0};
    BootstrapSource source;
    CupError err;
    char root_path[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];
    char script_path[MAX_PATH_LEN];
    char marker_path[MAX_PATH_LEN];
    unsigned long parent_pid;
    int root_is_directory;
    int marker_created = 0;

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

    err = bootstrap_find_uninstall(script_path, sizeof(script_path), &source);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: no valid installed or development uninstall script was found.\n");
        goto done;
    }
    (void)source;

    if (!confirm_uninstall(root_path)) {
        printf("Uninstall cancelled.\n");
        err = CUP_OK;
        goto done;
    }

    parent_pid = system_get_process_id();
    err = create_uninstall_marker(marker_path, sizeof(marker_path), parent_pid);
    if (err != CUP_OK) {
        goto done;
    }
    marker_created = 1;

    err = system_start_uninstall(root_path, script_path, parent_pid);
    if (err == CUP_OK) {
        printf("Uninstall started. The PATH entry was not removed.\n");
        marker_created = 0;
    }

done:
    if (marker_created) {
        system_remove_file(marker_path);
    }
    system_lock_release(&lock);
    return err;
}
