/*
 * Implements one package installation scope, including cache validation, staged extraction,
 * transaction persistence, state commit and selector-point reconciliation. The same scoped
 * operation is reused by stable updates.
 */

#include "package_install.h"
#include "installed_package.h"
#include "package_cache.h"

#include "command_context.h"
#include "runtime_journal.h"
#include "package_extract.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "filesystem.h"
#include "interrupt.h"
#include "layout.h"
#include "package_catalog.h"
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
    PackageRequest request;
    PackageIdentity package;
    char format[MAX_IDENTIFIER_LEN];
    char url[MAX_CATALOG_URL_LEN];
    char checksum_url[MAX_CATALOG_URL_LEN];
    char archive_path[MAX_PATH_LEN];
    char staging_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int staging_created;
    int package_moved;
    int journal_started;
    int made_active;
    int package_already_installed;
    int active_moved;
    InstallRequestKind kind;
    char expected_active[MAX_SELECTOR_LEN];
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

        if (package_identity_validate(identity) != CUP_OK) {
            return CUP_ERR_INCONSISTENT_STATE;
        }
        if (strcmp(identity->tool, tool) == 0) {
            *is_installed = 1;
            return CUP_OK;
        }
    }

    return CUP_OK;
}

static CupError resolve_archive_format(const PackageCatalog *catalog,
                                       const PackageIdentity *package,
                                       const char *format_override,
                                       char *format,
                                       size_t size) {
    CupError err;
    int supported;

    if (text_is_empty(format_override)) {
        return package_catalog_get_default_format(catalog,
                                                  format,
                                                  size,
                                                  package->component,
                                                  package->tool,
                                                  package->host_platform,
                                                  package->target_platform);
    }

    err = text_copy(format, size, format_override);
    if (err != CUP_OK) {
        return err;
    }

    err = package_catalog_has_format(catalog,
                                     package->component,
                                     package->tool,
                                     package->host_platform,
                                     package->target_platform,
                                     format,
                                     &supported);
    if (err != CUP_OK) {
        return err;
    }

    if (!supported) {
        fprintf(stderr,
                "Error: archive format '%s' is not available for '%s'.\n",
                format,
                package->tool);
        return CUP_ERR_NOT_AVAILABLE;
    }

    return CUP_OK;
}

/* Request and scope preparation. */
static CupError prepare_install(InstallOperation *operation,
                                const char *component,
                                const char *selector,
                                const char *target_override,
                                const char *format_override,
                                InstallRequestKind kind,
                                const char *expected_active) {
    CupError err;
    int version_available;

    /* Parse the request and acquire one exclusive, transaction-free command context. */
    operation->kind = kind;
    if (!text_is_empty(expected_active) && text_copy(operation->expected_active,
                                                     sizeof(operation->expected_active),
                                                     expected_active) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

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

    /* Updates revalidate that the originally selected scope still exists. */
    if (kind == INSTALL_REQUEST_UPDATE) {
        int scope_installed;

        err = update_scope_is_installed(
            operation, component, operation->request.selector.tool, &scope_installed);
        if (err != CUP_OK) {
            return err;
        }
        if (!scope_installed) {
            fprintf(stderr,
                    "Warning: update scope '%s:%s' for target '%s' is no "
                    "longer installed; skipping it.\n",
                    component,
                    operation->request.selector.tool,
                    operation->context.target_platform);
            return CUP_ERR_NOT_INSTALLED;
        }
    }

    /* Resolve a concrete identity and confirm that the catalog still offers its version. */
    err = command_context_load_catalog(&operation->context);
    if (err != CUP_OK) {
        return err;
    }

    err = package_request_resolve(&operation->context.catalog,
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

    err = package_catalog_has_version(&operation->context.catalog,
                                      operation->package.component,
                                      operation->package.tool,
                                      operation->package.host_platform,
                                      operation->package.target_platform,
                                      operation->package.version,
                                      &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr,
                "Error: version '%s' is not available for '%s' on host '%s', "
                "target '%s'.\n",
                operation->package.version,
                operation->package.tool,
                operation->package.host_platform,
                operation->package.target_platform);
        return CUP_ERR_NOT_AVAILABLE;
    }

    /* A valid existing package is reusable only for update-scope activation. */
    err = installed_package_require_absent(&operation->context.state, &operation->package);
    if (err == CUP_ERR_ALREADY_INSTALLED) {
        if (kind == INSTALL_REQUEST_UPDATE) {
            err = installed_package_require_valid(&operation->context.state, &operation->package);
            if (err != CUP_OK) {
                return err;
            }
            operation->package_already_installed = 1;
            return CUP_OK;
        }

        fprintf(stderr,
                "Error: package '%s:%s@%s' is already installed for host '%s', "
                "target '%s'.\n",
                operation->package.component,
                operation->package.tool,
                operation->package.version,
                operation->package.host_platform,
                operation->package.target_platform);
        return err;
    }
    if (err != CUP_OK) {
        return err;
    }

    /* Allocate identity-bound staging and persist the transaction before downloading. */
    err = resolve_archive_format(&operation->context.catalog,
                                 &operation->package,
                                 format_override,
                                 operation->format,
                                 sizeof(operation->format));
    if (err != CUP_OK) {
        return err;
    }

    err = layout_create_staging_dir(operation->staging_path,
                                    sizeof(operation->staging_path),
                                    kind == INSTALL_REQUEST_UPDATE ? "update" : "install",
                                    &operation->package);
    if (err != CUP_OK) {
        return err;
    }
    operation->staging_created = 1;

    err = layout_build_install_path(
        operation->install_path, sizeof(operation->install_path), &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = package_transaction_begin(kind == INSTALL_REQUEST_UPDATE ? PACKAGE_OPERATION_UPDATE
                                                                   : PACKAGE_OPERATION_INSTALL,
                                    &operation->package,
                                    operation->staging_path);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                    "Error: transaction journal was created, but its durability "
                    "could not be confirmed. Run 'cup repair'.\n");
        }
        return err;
    }

    operation->journal_started = 1;
    return CUP_OK;
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
    err = package_extract_archive(
        operation->archive_path, operation->staging_path, operation->format);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    printf("==> Validating package...\n");
    return package_validate(operation->staging_path, &operation->package);
}

static CupError discard_invalid_cache(const InstallOperation *operation, CupError original_error) {
    CupError discard_error;

    discard_error = package_cache_discard(operation->archive_path);
    return discard_error == CUP_OK ? original_error : discard_error;
}

static CupError extract_install_package(InstallOperation *operation) {
    PackageCacheSource source;
    CupError err;

    printf("==> Resolving package archive for %s@%s...\n",
           operation->package.tool,
           operation->package.version);

    /* Resolve the archive and checksum endpoints from one concrete catalog identity. */
    err = package_catalog_build_url(&operation->context.catalog,
                                    operation->url,
                                    sizeof(operation->url),
                                    operation->package.component,
                                    operation->package.tool,
                                    operation->package.host_platform,
                                    operation->package.target_platform,
                                    operation->package.version,
                                    operation->format);
    if (err != CUP_OK) {
        return err;
    }
    err = package_catalog_build_checksum_url(&operation->context.catalog,
                                             operation->checksum_url,
                                             sizeof(operation->checksum_url),
                                             operation->package.component,
                                             operation->package.tool,
                                             operation->package.host_platform,
                                             operation->package.target_platform,
                                             operation->package.version);
    if (err != CUP_OK) {
        return err;
    }

    /* Prefer a verified cache entry, but record its source for one bounded refresh attempt. */
    err = package_cache_fetch(operation->archive_path,
                              sizeof(operation->archive_path),
                              operation->url,
                              operation->checksum_url,
                              &operation->package,
                              operation->format,
                              PACKAGE_CACHE_ALLOW,
                              &source);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (source == PACKAGE_CACHE_SOURCE_CACHE) {
        printf("==> Using cached package archive.\n");
    } else {
        printf("==> Downloaded package archive.\n");
    }

    /* Extract into staging and validate the package before any canonical path is touched. */
    err = extract_and_validate_package(operation);
    if (err != CUP_OK && source == PACKAGE_CACHE_SOURCE_CACHE &&
        package_failure_allows_refresh(err)) {
        printf("==> Cached package is invalid; downloading it again...\n");

        err = package_cache_discard(operation->archive_path);
        if (err != CUP_OK) {
            return err;
        }

        err = reset_install_staging(operation);
        if (err != CUP_OK) {
            return err;
        }

        err = package_cache_fetch(operation->archive_path,
                                  sizeof(operation->archive_path),
                                  operation->url,
                                  operation->checksum_url,
                                  &operation->package,
                                  operation->format,
                                  PACKAGE_CACHE_REFRESH,
                                  &source);
        if (err != CUP_OK) {
            return err;
        }

        printf("==> Downloaded replacement package archive.\n");
        err = extract_and_validate_package(operation);
    }

    /* Network data that still fails package validation must not remain cached. */
    if (err != CUP_OK) {
        if (source == PACKAGE_CACHE_SOURCE_NETWORK && package_failure_allows_refresh(err)) {
            return discard_invalid_cache(operation, err);
        }
        return err;
    }

    /* Freeze package metadata and prepare the canonical parent only after validation succeeds. */
    err = package_set_metadata_read_only(operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    return layout_ensure_package_parent(&operation->package);
}

/* Candidate state and selector-point planning. */
static CupError prepare_active_change(InstallOperation *operation,
                                      CupState *candidate,
                                      int package_is_new) {
    const PackageIdentity *current_active;
    PackageScope scope;
    char current_entry[MAX_SELECTOR_LEN];
    int should_set_active = 0;
    CupError err;

    err = package_identity_get_scope(&operation->package, &scope);
    if (err != CUP_OK) {
        return err;
    }
    current_active = state_get_active(candidate, &scope);
    if (current_active != NULL &&
        package_identity_format_selector(current_active, current_entry, sizeof(current_entry)) !=
            CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    if (current_active == NULL && package_is_new) {
        should_set_active = 1;
        operation->made_active = 1;
    } else if (operation->kind == INSTALL_REQUEST_UPDATE && operation->expected_active[0] != '\0' &&
               current_active != NULL && strcmp(current_entry, operation->expected_active) == 0 &&
               !package_identity_equals(current_active, &operation->package)) {
        should_set_active = 1;
        operation->active_moved = 1;
    }

    if (!should_set_active) {
        return CUP_OK;
    }

    err = state_set_active(candidate, &operation->package);
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

static CupError save_active_change(InstallOperation *operation, const CupState *candidate) {
    CupError err;

    if (!operation->wrappers_ready) {
        return CUP_OK;
    }

    operation->context.state = *candidate;
    err = state_save(&operation->context.state);
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

    err = prepare_active_change(operation, &candidate, 0);
    if (err != CUP_OK) {
        return err;
    }
    return save_active_change(operation, &candidate);
}

static CupError commit_install(InstallOperation *operation) {
    CupState candidate;
    CupError err;
    int cleanup_failed = 0;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;

    printf("==> Committing installation...\n");

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
    err = state_add_installed(&candidate, &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_active_change(operation, &candidate, 1);
    if (err != CUP_OK) {
        return err;
    }

    operation->context.state = candidate;
    err = state_save(&operation->context.state);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                    "Error: installation state was applied, but its durability "
                    "could not be confirmed. Run 'cup repair'.\n");
        }
        return err;
    }

    err = package_transaction_clear();
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
        err = package_transaction_clear();
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->journal_started = 0;
    }

    return CUP_OK;
}

static void print_install_result(const InstallOperation *operation) {
    printf("Installed %s ", operation->package.component);
    package_request_print(stdout, &operation->request);
    printf(" for host '%s', target '%s'%s.\n",
           operation->package.host_platform,
           operation->package.target_platform,
           operation->made_active ? " and set it as the first default" : "");
}

/* Shared one-scope execution used by install and update. */
static CupError run_install(InstallOperation *operation,
                            const char *component,
                            const char *selector,
                            const char *target_override,
                            const char *format_override,
                            InstallRequestKind kind,
                            const char *expected_active) {
    CupError err;

    err = prepare_install(
        operation, component, selector, target_override, format_override, kind, expected_active);
    if (err != CUP_OK) {
        if (operation->staging_created && err != CUP_ERR_COMMIT &&
            rollback_install(operation) != CUP_OK) {
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

    if (err != CUP_OK && err != CUP_ERR_COMMIT && operation->staging_created) {
        if (rollback_install(operation) != CUP_OK) {
            fprintf(stderr,
                    "Error: installation failed and rollback could not be "
                    "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
    }

done:
    command_context_end(&operation->context);
    return err;
}

/* Public command wrappers. */
CupError package_install(const char *component,
                         const char *selector,
                         const char *target_override,
                         const char *format_override) {
    InstallOperation operation = {0};
    CupError err;

    wrapper_plan_init(&operation.wrappers);
    err = run_install(&operation,
                      component,
                      selector,
                      target_override,
                      format_override,
                      INSTALL_REQUEST_USER,
                      NULL);
    if (err == CUP_OK) {
        print_install_result(&operation);
    }
    wrapper_plan_free(&operation.wrappers);
    return err;
}

CupError package_install_update_scope(const char *component,
                                      const char *tool,
                                      const char *target_override,
                                      const char *expected_active,
                                      int *installed,
                                      int *active_moved) {
    InstallOperation operation = {0};
    CupError err;
    char selector[MAX_SELECTOR_LEN];

    if (installed == NULL || active_moved == NULL || text_is_empty(component) ||
        text_is_empty(tool)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *installed = 0;
    *active_moved = 0;

    err = package_selector_format_parts(selector, sizeof(selector), tool, "stable");
    if (err != CUP_OK) {
        return err;
    }

    wrapper_plan_init(&operation.wrappers);
    err = run_install(&operation,
                      component,
                      selector,
                      target_override,
                      NULL,
                      INSTALL_REQUEST_UPDATE,
                      expected_active);
    if (err == CUP_OK) {
        *installed = !operation.package_already_installed;
        *active_moved = operation.active_moved || operation.made_active;
    }
    wrapper_plan_free(&operation.wrappers);
    return err;
}
