/*
 * Implements staged package removal. state.txt is the removal commit point; rollback restores
 * the staged package only while the old state is still authoritative.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "runtime_journal.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"

#include <stdio.h>
#include <string.h>

/* Removal state retained across staging, commit and rollback. */
typedef struct {
    CommandContext context;
    PackageRequest request;
    PackageIdentity package;
    char install_path[MAX_PATH_LEN];
    char staging_path[MAX_PATH_LEN];
    int package_moved;
    int journal_started;
    int removed_active;
    WrapperPlan wrappers;
    int wrappers_ready;
} RemoveOperation;

static CupError prepare_remove(RemoveOperation *operation,
                               const char *component,
                               const char *selector,
                               const char *target_override) {
    CupError err;

    /* Parse and resolve the request under an exclusive, transaction-free context. */
    err = package_request_parse(component, selector, &operation->request);
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_begin(&operation->context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }

    err = runtime_journal_require_none();
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_load_state(&operation->context);
    if (err != CUP_OK) {
        return err;
    }

    if (package_release_is_stable(operation->request.selector.release)) {
        err = command_context_load_catalog(&operation->context);
        if (err != CUP_OK) {
            return err;
        }
    }

    err =
        package_request_resolve(operation->context.has_catalog ? &operation->context.catalog : NULL,
                                component,
                                operation->context.host_platform,
                                operation->context.target_platform,
                                &operation->request);
    if (err != CUP_OK) {
        return err;
    }

    err = package_identity_init(&operation->package,
                                component,
                                operation->request.selector.tool,
                                operation->context.host_platform,
                                operation->context.target_platform,
                                operation->request.resolved_release);
    if (err != CUP_OK) {
        return err;
    }

    /* Removal requires one concrete package present in both state and the components tree. */
    err = installed_package_require_present(&operation->context.state, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    /* Persist identity-bound staging before moving the canonical package. */
    err = layout_build_install_path(
        operation->install_path, sizeof(operation->install_path), &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_make_staging_path(
        operation->staging_path, sizeof(operation->staging_path), "remove", &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = package_transaction_begin(
        PACKAGE_OPERATION_REMOVE, &operation->package, operation->staging_path);
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

/* Move the canonical package into identity-bound staging. */
static CupError stage_removal(RemoveOperation *operation) {
    CupError err;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    printf("==> Moving package to temporary storage...\n");

    err = system_move_path(operation->install_path, operation->staging_path, &commit_state);
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

/* Save the candidate state, then complete deletion after commit. */
static CupError commit_removal(RemoveOperation *operation) {
    CupError err;
    PackageScope scope;
    const PackageIdentity *active_identity;
    int cleanup_failed = 0;

    err = package_identity_get_scope(&operation->package, &scope);
    if (err != CUP_OK) {
        return err;
    }
    active_identity = state_get_active(&operation->context.state, &scope);
    operation->removed_active =
        active_identity != NULL && package_identity_equals(active_identity, &operation->package);

    err = state_clear_matching_active(&operation->context.state, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = state_remove_installed(&operation->context.state, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    if (operation->removed_active) {
        err = wrapper_plan_build(&operation->wrappers, &operation->context.state);
        if (err != CUP_OK) {
            return err;
        }
        operation->wrappers_ready = 1;
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
        cleanup_failed = 1;
    }

    if (package_transaction_clear() != CUP_OK) {
        fprintf(stderr,
                "Warning: package removal committed, but transaction cleanup "
                "failed. Run 'cup repair'.\n");
        cleanup_failed = 1;
    } else {
        operation->journal_started = 0;
    }

    if (operation->wrappers_ready && wrapper_plan_apply(&operation->wrappers) != CUP_OK) {
        fprintf(stderr,
                "Error: removal was saved, but managed wrappers could not "
                "be rebuilt. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }

    return cleanup_failed ? CUP_ERR_COMMIT : CUP_OK;
}

/* Restore the staged package only while the old state remains authoritative. */
static CupError rollback_removal(RemoveOperation *operation) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    if (operation->package_moved) {
        err = system_move_path(operation->staging_path, operation->install_path, &commit_state);
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->package_moved = 0;
    }

    if (operation->journal_started) {
        err = package_transaction_clear();
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->journal_started = 0;
    }

    return CUP_OK;
}

static void print_remove_result(const RemoveOperation *operation) {
    printf("Removed %s ", operation->package.component);
    package_request_print(stdout, &operation->request);
    printf(" for host '%s', target '%s'.\n",
           operation->package.host_platform,
           operation->package.target_platform);
}

/* Public remove command. */
CupError command_remove(const char *component, const char *selector, const char *target_override) {
    RemoveOperation operation = {0};
    CupError err;

    err = prepare_remove(&operation, component, selector, target_override);
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
    wrapper_plan_free(&operation.wrappers);
    command_context_end(&operation.context);
    return err;
}
