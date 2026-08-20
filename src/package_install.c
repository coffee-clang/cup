/*
 * Implements one package installation scope, including cache validation, staged extraction,
 * transaction persistence, state commit and managed-wrapper reconciliation. The same scoped
 * operation is reused by stable updates.
 */

#include "package_install.h"
#include "installed_package.h"
#include "package_cache.h"

#include "command_context.h"
#include "package_extract.h"
#include "runtime_journal.h"
#include "wrappers.h"
#include "filesystem.h"
#include "interrupt.h"
#include "layout.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "package_transaction.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Operation state shared across preparation, commit and rollback. */
typedef enum {
    INSTALL_REQUEST_USER,
    INSTALL_REQUEST_UPDATE
} InstallRequestKind;

typedef struct {
    CommandContext context;
    PackageArtifactSpec artifact_spec;
    VerifiedArtifact artifact;
    char staging_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int staging_created;
    int package_moved;
    int journal_started;
    SystemPathIdentity journal_identity;
    int made_default;
    int package_already_installed;
    int default_moved;
    InstallRequestKind kind;
    PackageIdentity expected_default;
    int has_expected_default;
    WrapperPlan wrappers;
    int wrappers_ready;
} InstallOperation;

static CupError update_scope_is_installed(const InstallOperation *operation,
                                          const char *component,
                                          const char *tool,
                                          int *is_installed) {
    size_t i;

    if (operation == NULL || text_is_empty(component) || text_is_empty(tool) ||
        is_installed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    *is_installed = 0;
    for (i = 0; i < operation->context.state.installed_count; ++i) {
        const PackageIdentity *identity = &operation->context.state.installed[i];

        if (strcmp(identity->component, component) != 0 ||
            strcmp(identity->host_platform, operation->context.host_platform) != 0 ||
            strcmp(identity->target_platform, operation->context.target_platform) != 0) {
            continue;
        }

        if (package_identity_validate(identity, stderr) != CUP_OK) {
            return CUP_ERR_INCONSISTENT_STATE;
        }
        if (strcmp(identity->tool, tool) == 0) {
            *is_installed = 1;
            return CUP_OK;
        }
    }

    return CUP_OK;
}

static CupError prepare_install_operation(InstallOperation *operation,
                                          const PackageArtifactSpec *spec,
                                          InstallRequestKind kind,
                                          const PackageIdentity *expected_default) {
    if (operation == NULL || spec == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    operation->kind = kind;
    operation->artifact_spec = *spec;

    if (expected_default != NULL) {
        if (package_identity_validate(expected_default, NULL) != CUP_OK) {
            return CUP_ERR_INVALID_INPUT;
        }
        operation->expected_default = *expected_default;
        operation->has_expected_default = 1;
    }
    return CUP_OK;
}

static CupError load_install_context(InstallOperation *operation) {
    CupError err;

    err = command_context_begin(&operation->context,
                                operation->artifact_spec.identity.target_platform,
                                SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) {
        err = command_context_load_state(&operation->context);
    }
    if (err != CUP_OK) {
        return err;
    }

    if (strcmp(operation->context.host_platform,
               operation->artifact_spec.identity.host_platform) != 0 ||
        strcmp(operation->context.target_platform,
               operation->artifact_spec.identity.target_platform) != 0) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    return CUP_OK;
}

static CupError validate_update_scope(InstallOperation *operation) {
    CupError err;
    int scope_installed;

    if (operation->kind != INSTALL_REQUEST_UPDATE) {
        return CUP_OK;
    }

    err = update_scope_is_installed(operation,
                                    operation->artifact_spec.identity.component,
                                    operation->artifact_spec.identity.tool,
                                    &scope_installed);
    if (err != CUP_OK) {
        return err;
    }
    if (scope_installed) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Warning: installed package '%s:%s' for target '%s' is no "
            "longer installed; skipping it.\n",
            operation->artifact_spec.identity.component,
            operation->artifact_spec.identity.tool,
            operation->context.target_platform);
    return CUP_ERR_NOT_INSTALLED;
}

static CupError check_existing_install(InstallOperation *operation, int *complete) {
    CupError err;

    *complete = 0;
    err = installed_package_require_absent(&operation->context.state,
                                           &operation->artifact_spec.identity);
    if (err != CUP_ERR_ALREADY_INSTALLED) {
        return err;
    }

    if (operation->kind == INSTALL_REQUEST_UPDATE) {
        err = installed_package_require_valid(&operation->context.state,
                                              &operation->artifact_spec.identity);
        if (err != CUP_OK) {
            return err;
        }
        operation->package_already_installed = 1;
        *complete = 1;
        return CUP_OK;
    }

    printf("Package '%s:%s@%s' is already installed for host '%s', "
           "target '%s'; no changes were made.\n",
           operation->artifact_spec.identity.component,
           operation->artifact_spec.identity.tool,
           operation->artifact_spec.identity.version,
           operation->artifact_spec.identity.host_platform,
           operation->artifact_spec.identity.target_platform);
    return CUP_ERR_ALREADY_INSTALLED;
}

static CupError prepare_install_staging(InstallOperation *operation) {
    CupError err;

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }

    err = layout_create_staging_dir(
        operation->staging_path,
        sizeof(operation->staging_path),
        operation->kind == INSTALL_REQUEST_UPDATE ? "update" : "install",
        &operation->artifact_spec.identity);
    if (err != CUP_OK) {
        return err;
    }
    operation->staging_created = 1;

    err = layout_build_install_path(operation->install_path,
                                    sizeof(operation->install_path),
                                    &operation->artifact_spec.identity);
    return err;
}

static CupError begin_install_commit(InstallOperation *operation) {
    PackageTransaction journal;
    CupError err;

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }
    err = package_transaction_begin(
        operation->kind == INSTALL_REQUEST_UPDATE ? PACKAGE_OPERATION_UPDATE
                                                  : PACKAGE_OPERATION_INSTALL,
        &operation->artifact_spec.identity,
        operation->staging_path,
        &journal);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                    "Error: transaction journal was created, but its durability "
                    "could not be confirmed. Run 'cup repair'.\n");
        }
        return err;
    }

    operation->journal_identity = journal.file_identity;
    operation->journal_started = 1;
    return CUP_OK;
}

static CupError prepare_install(InstallOperation *operation,
                                const PackageArtifactSpec *spec,
                                InstallRequestKind kind,
                                const PackageIdentity *expected_default) {
    CupError err;
    int complete;

    err = prepare_install_operation(operation, spec, kind, expected_default);
    if (err == CUP_OK) {
        err = load_install_context(operation);
    }
    if (err == CUP_OK) {
        err = validate_update_scope(operation);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = check_existing_install(operation, &complete);
    if (err != CUP_OK || complete) {
        return err;
    }

    return prepare_install_staging(operation);
}


/* Archive extraction, cache refresh and package validation. */
static int package_failure_allows_refresh(CupError err) {
    return err == CUP_ERR_ARCHIVE || err == CUP_ERR_ARCHIVE_UNSAFE || err == CUP_ERR_VALIDATION;
}

static CupError reset_install_staging(InstallOperation *operation) {
    CupError err;

    err = filesystem_remove_tree(operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    return filesystem_ensure_directory(operation->staging_path);
}

static CupError extract_and_validate_package(InstallOperation *operation) {
    CupError err;

    printf("==> Extracting package...\n");
    err = package_extract_verified(&operation->artifact, operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    printf("==> Validating package...\n");
    return package_validate(operation->staging_path, &operation->artifact_spec.identity, stderr);
}

static CupError discard_invalid_cache(InstallOperation *operation, CupError original_error) {
    CupError discard_error;

    discard_error = verified_artifact_discard(&operation->artifact);
    return discard_error == CUP_OK ? original_error : discard_error;
}

static CupError extract_install_package(InstallOperation *operation) {
    PackageCacheResult cache_result;
    CupError err;

    printf("==> Resolving package archive for %s@%s...\n",
           operation->artifact_spec.identity.tool,
           operation->artifact_spec.identity.version);

    err = package_cache_fetch_artifact(&operation->artifact,
                                       &operation->artifact_spec,
                                       PACKAGE_CACHE_ALLOW,
                                       &cache_result);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (cache_result.source == PACKAGE_CACHE_SOURCE_CACHE) {
        printf("==> Using cached package archive.\n");
    } else {
        printf("==> Downloaded package archive.\n");
    }

    err = extract_and_validate_package(operation);
    if (err != CUP_OK && cache_result.source == PACKAGE_CACHE_SOURCE_CACHE &&
        package_failure_allows_refresh(err)) {
        printf("==> Cached package is invalid; downloading it again...\n");

        err = verified_artifact_discard(&operation->artifact);
        if (err != CUP_OK) {
            return err;
        }
        err = reset_install_staging(operation);
        if (err != CUP_OK) {
            return err;
        }
        err = package_cache_fetch_artifact(&operation->artifact,
                                           &operation->artifact_spec,
                                           PACKAGE_CACHE_REFRESH,
                                           &cache_result);
        if (err != CUP_OK) {
            return err;
        }

        printf("==> Downloaded replacement package archive.\n");
        err = extract_and_validate_package(operation);
    }

    if (err != CUP_OK) {
        if (cache_result.source == PACKAGE_CACHE_SOURCE_NETWORK &&
            package_failure_allows_refresh(err)) {
            return discard_invalid_cache(operation, err);
        }
        return err;
    }

    verified_artifact_release(&operation->artifact);
    err = package_set_metadata_read_only(operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    return layout_ensure_package_parent(&operation->artifact_spec.identity);
}

/* Build the candidate state and its complete managed-wrapper plan before commit. */
static CupError prepare_default_change(InstallOperation *operation,
                                       CupState *candidate,
                                       int package_is_new) {
    const PackageIdentity *current_default;
    PackageScope scope;
    int should_set_default = 0;
    CupError err;

    err = package_identity_get_scope(&operation->artifact_spec.identity, &scope);
    if (err != CUP_OK) {
        return err;
    }
    current_default = state_get_default(candidate, &scope);

    if (current_default == NULL && package_is_new) {
        should_set_default = 1;
        operation->made_default = 1;
    } else if (operation->kind == INSTALL_REQUEST_UPDATE &&
               operation->has_expected_default && current_default != NULL &&
               package_identity_equals(current_default, &operation->expected_default) &&
               !package_identity_equals(current_default, &operation->artifact_spec.identity)) {
        should_set_default = 1;
        operation->default_moved = 1;
    }

    if (!should_set_default) {
        return CUP_OK;
    }

    err = state_set_default(candidate, &operation->artifact_spec.identity);
    if (err != CUP_OK) {
        return err;
    }

    err = wrapper_plan_build(&operation->wrappers, candidate);
    if (err != CUP_OK) {
        return err;
    }
    operation->wrappers_ready = 1;
    return CUP_OK;
}

static CupError save_default_change(InstallOperation *operation, const CupState *candidate) {
    CupError err;

    if (!operation->wrappers_ready) {
        return CUP_OK;
    }

    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }
    operation->context.state = *candidate;
    err = state_save(&operation->context.state,
                     &operation->context.state_identity,
                     &operation->context.state_identity);
    if (err != CUP_OK) {
        return err;
    }

    err = wrapper_plan_apply(&operation->wrappers);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: the updated default was saved, but its wrappers "
                "could not be rebuilt. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

/* Commit and rollback. state.txt is the package commit point. */
static CupError commit_existing_update(InstallOperation *operation) {
    CupState candidate = operation->context.state;
    CupError err;

    err = prepare_default_change(operation, &candidate, 0);
    if (err != CUP_OK) {
        return err;
    }
    return save_default_change(operation, &candidate);
}

static CupError commit_install(InstallOperation *operation) {
    CupState candidate;
    CupError err;
    int cleanup_failed = 0;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    printf("==> Committing installation...\n");

    err = begin_install_commit(operation);
    if (err != CUP_OK) {
        return err;
    }
    err = interrupt_safe_point();
    if (err != CUP_OK) {
        return err;
    }
    err = system_move_path(operation->staging_path, operation->install_path, &commit_state);
    if (err != CUP_OK) {
        if (commit_state == SYSTEM_COMMIT_APPLIED) {
            operation->package_moved = 1;
            fprintf(stderr,
                    "Error: package was moved into place, but its durability could "
                    "not be confirmed. Run 'cup repair'.\n");
            return CUP_ERR_COMMIT;
        }
        return err;
    }
    operation->package_moved = 1;

    candidate = operation->context.state;
    err = state_add_installed(&candidate, &operation->artifact_spec.identity);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_default_change(operation, &candidate, 1);
    if (err != CUP_OK) {
        return err;
    }

    operation->context.state = candidate;
    err = state_save(&operation->context.state,
                     &operation->context.state_identity,
                     &operation->context.state_identity);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                    "Error: installation state was applied, but its durability "
                    "could not be confirmed. Run 'cup repair'.\n");
        }
        return err;
    }

    err = runtime_journal_clear_if_identity(&operation->journal_identity);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Warning: installation committed, but transaction cleanup failed. "
                "Run 'cup repair'.\n");
        cleanup_failed = 1;
    } else {
        operation->journal_started = 0;
    }

    if (operation->wrappers_ready) {
        err = wrapper_plan_apply(&operation->wrappers);
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Error: installation and its default were saved, but selector "
                    "points could not be rebuilt. Run 'cup repair'.\n");
            return CUP_ERR_COMMIT;
        }
    }

    return cleanup_failed ? CUP_ERR_COMMIT : CUP_OK;
}

static CupError rollback_install(InstallOperation *operation) {
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    CupError err;

    if (operation->package_moved) {
        err = system_move_path(operation->install_path, operation->staging_path, &commit_state);
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->package_moved = 0;
    }

    err = filesystem_remove_tree(operation->staging_path);
    if (err != CUP_OK) {
        return CUP_ERR_ROLLBACK;
    }
    operation->staging_created = 0;

    if (operation->journal_started) {
        err = runtime_journal_clear_if_identity(&operation->journal_identity);
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->journal_started = 0;
    }

    return CUP_OK;
}

static void print_install_result(const InstallOperation *operation) {
    printf("Installed %s %s@%s for host '%s', target '%s'%s.\n",
           operation->artifact_spec.identity.component,
           operation->artifact_spec.identity.tool,
           operation->artifact_spec.identity.version,
           operation->artifact_spec.identity.host_platform,
           operation->artifact_spec.identity.target_platform,
           operation->made_default ? " and set it as the first default" : "");
}

/* Shared one-scope execution used by install and update. */
static CupError execute_install(InstallOperation *operation,
                                const PackageArtifactSpec *spec,
                                InstallRequestKind kind,
                                const PackageIdentity *expected_default) {
    CupError err;

    err = prepare_install(operation, spec, kind, expected_default);
    if (err != CUP_OK) {
        if (operation->staging_created && rollback_install(operation) != CUP_OK) {
            fprintf(stderr,
                    "Error: installation failed and rollback could not be "
                    "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
        goto done;
    }

    if (operation->package_already_installed) {
        err = commit_existing_update(operation);
    } else {
        err = extract_install_package(operation);
        if (err == CUP_OK) {
            err = commit_install(operation);
        }
    }

    if (err != CUP_OK && operation->staging_created &&
        (err != CUP_ERR_COMMIT || !operation->package_moved)) {
        if (rollback_install(operation) != CUP_OK) {
            fprintf(stderr,
                    "Error: installation failed and rollback could not be "
                    "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
    }

done:
    verified_artifact_release(&operation->artifact);
    command_context_end(&operation->context);
    return err;
}

/* Package installation entry points. */
CupError package_install_artifact(const PackageArtifactSpec *spec) {
    InstallOperation operation = {0};
    CupError err;

    if (spec == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    wrapper_plan_init(&operation.wrappers);
    verified_artifact_init(&operation.artifact);
    err = execute_install(
        &operation, spec, INSTALL_REQUEST_USER, NULL);
    if (err == CUP_OK) {
        print_install_result(&operation);
    }
    wrapper_plan_free(&operation.wrappers);
    return err;
}

CupError package_install_update_artifact(const PackageArtifactSpec *spec,
                                         const PackageIdentity *expected_default,
                                         int *installed,
                                         int *default_moved) {
    InstallOperation operation = {0};
    CupError err;

    if (spec == NULL || installed == NULL || default_moved == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *installed = 0;
    *default_moved = 0;

    wrapper_plan_init(&operation.wrappers);
    verified_artifact_init(&operation.artifact);
    err = execute_install(
        &operation, spec, INSTALL_REQUEST_UPDATE, expected_default);
    if (err == CUP_OK) {
        *installed = !operation.package_already_installed;
        *default_moved = operation.default_moved || operation.made_default;
    }
    wrapper_plan_free(&operation.wrappers);
    return err;
}
