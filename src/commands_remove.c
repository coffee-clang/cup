#include "commands.h"

#include "command_context.h"
#include "entry.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "transaction.h"

#include <stdio.h>

// REMOVE COMMAND
CupError handle_remove(const char *component, const char *entry, const char *target_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    CupError err;
    char pid[MAX_NAME_LEN];
    char install_path[MAX_PATH_LEN];
    char temporary_path[MAX_PATH_LEN];
    int package_moved = 0;
    int state_changed = 0;

    err = entry_request_parse(component, entry, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_no_transaction();
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    if (entry_is_stable(request.release)) {
        err = command_context_load_manifest(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = entry_request_resolve(context.has_manifest ? &context.manifest : NULL,
        component, context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform, request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_installed(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = system_get_process_id(pid, sizeof(pid));
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_tmp_path(temporary_path, sizeof(temporary_path),
        "remove", &package, pid);
    if (err != CUP_OK) {
        goto done;
    }

    err = filesystem_remove_tree(temporary_path);
    if (err != CUP_OK) {
        goto done;
    }

    err = transaction_begin(TRANSACTION_REMOVE, &package, temporary_path);
    if (err != CUP_OK) {
        goto done;
    }

    printf("==> Moving package to temporary storage...\n");

    err = system_move_path(install_path, temporary_path);
    if (err != CUP_OK) {
        goto rollback;
    }
    package_moved = 1;

    err = state_clear_matching_default(&context.state, component,
        package.host_platform, package.target_platform, request.resolved_entry);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = state_remove_installed(&context.state, component,
        package.host_platform, package.target_platform, request.resolved_entry);
    if (err != CUP_OK) {
        goto rollback;
    }
    state_changed = 1;

    err = state_save(&context.state);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = filesystem_remove_tree(temporary_path);
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: package was removed from state, but temporary cleanup failed. "
            "Run 'cup repair'.\n");
        err = CUP_OK;
        goto done;
    }

    err = transaction_clear();
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: package removal committed, but transaction cleanup failed. "
            "Run 'cup repair'.\n");
        err = CUP_OK;
    }

    printf("Removed %s ", component);
    entry_request_print(stdout, &request);
    printf(" for host '%s', target '%s'.\n",
        package.host_platform, package.target_platform);
    goto done;

rollback:
    if (state_changed) {
        state_add_installed(&context.state, component,
            package.host_platform, package.target_platform, request.resolved_entry);
    }

    if (package_moved && system_move_path(temporary_path, install_path) != CUP_OK) {
        fprintf(stderr, "Error: removal failed and rollback could not be completed. "
            "Run 'cup repair'.\n");
        err = CUP_ERR_ROLLBACK;
        goto done;
    }

    transaction_clear();

done:
    command_context_end(&context);
    return err;
}
