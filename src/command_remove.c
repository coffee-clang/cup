/*
 * Implements staged package removal. state.txt is the removal commit point; rollback restores
 * the staged package only while the old state is still authoritative.
 */

#include "commands.h"
#include "installed_package.h"
#include "interrupt.h"

#include "command_context.h"
#include "package_selector.h"
#include "package_request.h"
#include "runtime_journal.h"
#include "wrappers.h"
#include "filesystem.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "text.h"

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
    SystemPathIdentity journal_identity;
    int removed_default;
    WrapperPlan wrappers;
    int wrappers_ready;
} RemoveOperation;

static CupError parse_remove_selection(const char *component,
                                       const char *selector,
                                       char *tool,
                                           size_t tool_size,
                                           char *release,
                                           size_t release_size,
                                           int *release_omitted) {
    CupError err;

    if (text_is_empty(component) || text_is_empty(selector) || tool == NULL || tool_size == 0 ||
        release == NULL || release_size == 0 || release_omitted == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *release_omitted = strchr(selector, '@') == NULL;
    if (*release_omitted) {
        err = text_copy(tool, tool_size, selector);
        release[0] = '\0';
    } else {
        err = package_selector_parse_parts(selector, tool, tool_size, release, release_size);
    }
    return err;
}

static int installed_identity_matches(const PackageIdentity *candidate,
                                      const char *component,
                                      const char *tool,
                                      const char *host,
                                      const char *target) {
    return candidate != NULL && strcmp(candidate->component, component) == 0 &&
           strcmp(candidate->tool, tool) == 0 &&
           strcmp(candidate->host_platform, host) == 0 &&
           strcmp(candidate->target_platform, target) == 0;
}

static CupError resolve_unique_installed_release(const CupState *state,
                                                 const char *component,
                                                 const char *tool,
                                                 const char *host,
                                                 const char *target,
                                                 char *release,
                                                 size_t release_size) {
    const PackageIdentity *match = NULL;
    size_t match_count = 0;
    size_t i;

    if (state == NULL || text_is_empty(component) || text_is_empty(tool) ||
        text_is_empty(host) || text_is_empty(target) || release == NULL || release_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }

    for (i = 0; i < state->installed_count; ++i) {
        if (installed_identity_matches(
                &state->installed[i], component, tool, host, target)) {
            match = &state->installed[i];
            match_count++;
        }
    }

    if (match_count == 0) {
        fprintf(stderr,
                "Error: no installed release matches '%s:%s' for host '%s', target '%s'.\n",
                component,
                tool,
                host,
                target);
        return CUP_ERR_NOT_INSTALLED;
    }
    if (match_count > 1) {
        fprintf(stderr,
                "Error: remove selection '%s:%s' is ambiguous for host '%s', target '%s'.\n"
                "Installed releases:\n",
                component,
                tool,
                host,
                target);
        for (i = 0; i < state->installed_count; ++i) {
            if (installed_identity_matches(
                    &state->installed[i], component, tool, host, target)) {
                fprintf(stderr, "  %s@%s\n", tool, state->installed[i].version);
            }
        }
        fprintf(stderr,
                "Specify one of the installed releases with:\n"
                "  cup remove %s %s@<release> --target %s\n",
                component,
                tool,
                target);
        return CUP_ERR_INVALID_INPUT;
    }

    return text_copy(release, release_size, match->version);
}

static CupError prepare_remove(RemoveOperation *operation,
                               const char *component,
                               const char *selector,
                               const char *target_override) {
    CupError err;
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
    char normalized_selector[MAX_SELECTOR_LEN];
    int release_omitted;

    err = parse_remove_selection(
        component, selector, tool, sizeof(tool), release, sizeof(release), &release_omitted);
    if (err != CUP_OK) {
        return err;
    }

    /* Resolve the request under one exclusive, transaction-free state snapshot. */
    err = command_context_begin(&operation->context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_load_state(&operation->context);
    if (err != CUP_OK) {
        return err;
    }

    if (release_omitted) {
        err = resolve_unique_installed_release(&operation->context.state,
                                               component,
                                               tool,
                                               operation->context.host_platform,
                                               operation->context.target_platform,
                                               release,
                                               sizeof(release));
        if (err != CUP_OK) {
            return err;
        }
    }
    err = package_selector_format_parts(
        normalized_selector, sizeof(normalized_selector), tool, release);
    if (err == CUP_OK) {
        err = package_request_parse(component, normalized_selector, &operation->request);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (release_omitted) {
        err = text_copy(operation->request.input_selector,
                        sizeof(operation->request.input_selector),
                        tool);
        if (err != CUP_OK) {
            return err;
        }
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

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }

    {
        PackageTransaction journal;
        err = package_transaction_begin(
            PACKAGE_OPERATION_REMOVE, &operation->package, operation->staging_path, &journal);
        if (err == CUP_OK) {
            operation->journal_identity = journal.file_identity;
        }
    }
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

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }
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
    const PackageIdentity *default_identity;
    int cleanup_failed = 0;

    err = package_identity_get_scope(&operation->package, &scope);
    if (err != CUP_OK) {
        return err;
    }
    default_identity = state_get_default(&operation->context.state, &scope);
    operation->removed_default =
        default_identity != NULL && package_identity_equals(default_identity, &operation->package);

    err = state_clear_matching_default(&operation->context.state, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = state_remove_installed(&operation->context.state, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    if (operation->removed_default) {
        err = wrapper_plan_build(&operation->wrappers, &operation->context.state);
        if (err != CUP_OK) {
            return err;
        }
        operation->wrappers_ready = 1;
    }

    err = state_save(&operation->context.state,
                     &operation->context.state_identity,
                     &operation->context.state_identity);
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

    if (runtime_journal_clear_if_identity(&operation->journal_identity) != CUP_OK) {
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
        err = runtime_journal_clear_if_identity(&operation->journal_identity);
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
