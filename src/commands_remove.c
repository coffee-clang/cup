#include "commands.h"

#include "command_context.h"
#include "entry.h"
#include "entrypoints.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "transaction.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    CommandContext context;
    EntryRequest request;
    PackageIdentity package;
    char install_path[MAX_PATH_LEN];
    char staging_path[MAX_PATH_LEN];
    int package_moved;
    int journal_started;
    int removed_default;
    EntryPointPlan entrypoints;
    int entrypoints_ready;
} RemoveOperation;

static CupError prepare_remove(RemoveOperation *operation,
    const char *component, const char *entry, const char *target_override) {
    CupError err;

    err = entry_request_parse(component, entry, &operation->request);
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_begin(&operation->context, target_override,
        SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }

    err = command_require_no_transaction();
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_load_state(&operation->context);
    if (err != CUP_OK) {
        return err;
    }

    if (entry_is_stable(operation->request.release)) {
        err = command_context_load_manifest(&operation->context);
        if (err != CUP_OK) {
            return err;
        }
    }

    err = entry_request_resolve(operation->context.has_manifest
            ? &operation->context.manifest : NULL,
        component, operation->context.host_platform,
        operation->context.target_platform, &operation->request);
    if (err != CUP_OK) {
        return err;
    }

    err = package_identity_init(&operation->package, component,
        operation->request.tool, operation->context.host_platform,
        operation->context.target_platform,
        operation->request.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    err = command_require_installed(&operation->context,
        &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_build_install_path(operation->install_path,
        sizeof(operation->install_path), &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_make_tmp_path(operation->staging_path,
        sizeof(operation->staging_path), "remove", &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = transaction_begin(TRANSACTION_REMOVE, &operation->package,
        operation->staging_path);
    if (err == CUP_ERR_COMMIT) {
        fprintf(stderr,
            "Error: transaction journal was created, but its durability could "
            "not be confirmed. Run 'cup repair'.\n");
        return err;
    }
    if (err != CUP_OK) {
        return err;
    }

    operation->journal_started = 1;
    return CUP_OK;
}

static CupError stage_removal(RemoveOperation *operation) {
    CupError err;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    printf("==> Moving package to temporary storage...\n");

    err = system_move_path(operation->install_path, operation->staging_path,
        &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_APPLIED) {
        operation->package_moved = 1;
        fprintf(stderr,
            "Error: package was moved to temporary storage, but its durability "
            "could not be confirmed. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }

    if (err == CUP_OK) {
        operation->package_moved = 1;
    }

    return err;
}

static CupError commit_removal(RemoveOperation *operation) {
    CupError err;
    const char *default_entry;

    default_entry = state_get_default(&operation->context.state,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform);
    operation->removed_default = default_entry != NULL &&
        strcmp(default_entry, operation->request.resolved_entry) == 0;

    err = state_clear_matching_default(&operation->context.state,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform,
        operation->request.resolved_entry);
    if (err != CUP_OK) {
        return err;
    }

    err = state_remove_installed(&operation->context.state,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform,
        operation->request.resolved_entry);
    if (err != CUP_OK) {
        return err;
    }

    if (operation->removed_default) {
        err = entrypoint_plan_build(&operation->entrypoints,
            &operation->context.state);
        if (err != CUP_OK) {
            return err;
        }
        operation->entrypoints_ready = 1;
    }

    err = state_save(&operation->context.state);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                "Error: removal state was applied, but its durability could "
                "not be confirmed. Run 'cup repair'.\n");
        }
        return err;
    }

    err = filesystem_remove_tree(operation->staging_path);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Warning: package was removed from state, but temporary cleanup "
            "failed. Run 'cup repair'.\n");
    }

    if (transaction_clear() != CUP_OK) {
        fprintf(stderr,
            "Warning: package removal committed, but transaction cleanup "
            "failed. Run 'cup repair'.\n");
    } else {
        operation->journal_started = 0;
    }

    if (operation->entrypoints_ready &&
        entrypoint_plan_apply(&operation->entrypoints) != CUP_OK) {
        fprintf(stderr,
            "Error: removal was saved, but managed entry points could not "
            "be rebuilt. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }

    return CUP_OK;
}

static CupError rollback_removal(RemoveOperation *operation) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    if (operation->package_moved) {
        err = system_move_path(operation->staging_path,
            operation->install_path, &commit_state);
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->package_moved = 0;
    }

    if (operation->journal_started) {
        err = transaction_clear();
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->journal_started = 0;
    }

    return CUP_OK;
}

static void print_remove_result(const RemoveOperation *operation) {
    printf("Removed %s ", operation->package.component);
    entry_request_print(stdout, &operation->request);
    printf(" for host '%s', target '%s'.\n",
        operation->package.host_platform, operation->package.target_platform);
}

CupError command_remove(const char *component, const char *entry,
    const char *target_override) {
    RemoveOperation operation = {0};
    CupError err;

    err = prepare_remove(&operation, component, entry, target_override);
    if (err != CUP_OK) {
        goto done;
    }

    err = stage_removal(&operation);
    if (err == CUP_OK) {
        err = commit_removal(&operation);
    }

    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        if (rollback_removal(&operation) != CUP_OK) {
            fprintf(stderr,
                "Error: removal failed and rollback could not be completed. "
                "Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
        goto done;
    }

    if (err == CUP_OK) {
        print_remove_result(&operation);
    }

done:
    entrypoint_plan_free(&operation.entrypoints);
    command_context_end(&operation.context);
    return err;
}
