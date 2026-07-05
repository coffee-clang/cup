#include "commands.h"

#include "command_context.h"
#include "extract.h"
#include "entry.h"
#include "entrypoints.h"
#include "fetch.h"
#include "filesystem.h"
#include "interrupt.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "transaction.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    INSTALL_REQUEST_USER,
    INSTALL_REQUEST_UPDATE
} InstallRequestKind;

typedef struct {
    CommandContext context;
    EntryRequest request;
    PackageIdentity package;
    char format[MAX_NAME_LEN];
    char url[MAX_MANIFEST_URL_LEN];
    char checksum_url[MAX_MANIFEST_URL_LEN];
    char archive_path[MAX_PATH_LEN];
    char staging_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int staging_created;
    int package_moved;
    int journal_started;
    int made_default;
    int package_already_installed;
    int default_moved;
    InstallRequestKind kind;
    char expected_default[MAX_ENTRY_LEN];
    EntryPointPlan entrypoints;
    int entrypoints_ready;
} InstallOperation;

static CupError resolve_archive_format(const Manifest *manifest,
    const PackageIdentity *package, const char *format_override,
    char *format, size_t size) {
    CupError err;
    int supported;

    if (text_is_empty(format_override)) {
        return manifest_get_default_format(manifest, format, size,
            package->component, package->tool,
            package->host_platform, package->target_platform);
    }

    err = text_copy(format, size, format_override);
    if (err != CUP_OK) {
        return err;
    }

    err = manifest_has_format(manifest, package->component, package->tool,
        package->host_platform, package->target_platform, format, &supported);
    if (err != CUP_OK) {
        return err;
    }

    if (!supported) {
        fprintf(stderr,
            "Error: archive format '%s' is not available for '%s'.\n",
            format, package->tool);
        return CUP_ERR_NOT_AVAILABLE;
    }

    return CUP_OK;
}

static CupError prepare_install(InstallOperation *operation,
    const char *component, const char *entry, const char *target_override,
    const char *format_override, InstallRequestKind kind,
    const char *expected_default) {
    CupError err;
    int version_available;

    operation->kind = kind;
    if (!text_is_empty(expected_default) &&
        text_copy(operation->expected_default,
            sizeof(operation->expected_default), expected_default) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

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

    err = command_context_load_manifest(&operation->context);
    if (err != CUP_OK) {
        return err;
    }

    err = entry_request_resolve(&operation->context.manifest, component,
        operation->context.host_platform, operation->context.target_platform,
        &operation->request);
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

    err = manifest_has_version(&operation->context.manifest,
        operation->package.component, operation->package.tool,
        operation->package.host_platform, operation->package.target_platform,
        operation->package.version, &version_available);
    if (err != CUP_OK) {
        return err;
    }

    if (!version_available) {
        fprintf(stderr,
            "Error: version '%s' is not available for '%s' on host '%s', "
            "target '%s'.\n",
            operation->package.version, operation->package.tool,
            operation->package.host_platform, operation->package.target_platform);
        return CUP_ERR_NOT_AVAILABLE;
    }

    err = command_require_absent(&operation->context, &operation->package);
    if (err == CUP_ERR_ALREADY_INSTALLED) {
        if (kind == INSTALL_REQUEST_UPDATE) {
            err = command_require_valid_installed(&operation->context,
                &operation->package);
            if (err != CUP_OK) {
                return err;
            }
            operation->package_already_installed = 1;
            return CUP_OK;
        }

        fprintf(stderr,
            "Error: package '%s:%s@%s' is already installed for host '%s', "
            "target '%s'.\n",
            operation->package.component, operation->package.tool,
            operation->package.version, operation->package.host_platform,
            operation->package.target_platform);
        return err;
    }
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_archive_format(&operation->context.manifest,
        &operation->package, format_override, operation->format,
        sizeof(operation->format));
    if (err != CUP_OK) {
        return err;
    }

    err = layout_create_tmp_dir(operation->staging_path,
        sizeof(operation->staging_path), "install", &operation->package);
    if (err != CUP_OK) {
        return err;
    }
    operation->staging_created = 1;

    err = layout_build_install_path(operation->install_path,
        sizeof(operation->install_path), &operation->package);
    if (err != CUP_OK) {
        return err;
    }

    err = transaction_begin(TRANSACTION_INSTALL, &operation->package,
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

static int package_failure_allows_refresh(CupError err) {
    return err == CUP_ERR_ARCHIVE || err == CUP_ERR_ARCHIVE_UNSAFE ||
        err == CUP_ERR_VALIDATION;
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
    err = extract_archive(operation->archive_path, operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    printf("==> Validating package...\n");
    return package_validate(operation->staging_path, &operation->package);
}

static CupError discard_invalid_cache(const InstallOperation *operation,
    CupError original_error) {
    CupError discard_error;

    discard_error = fetch_discard_cached_package(operation->archive_path);
    return discard_error == CUP_OK ? original_error : discard_error;
}

static CupError extract_install_package(InstallOperation *operation) {
    FetchSource source;
    CupError err;

    printf("==> Resolving package archive for %s@%s...\n",
        operation->package.tool, operation->package.version);

    err = manifest_build_url(&operation->context.manifest,
        operation->url, sizeof(operation->url),
        operation->package.component, operation->package.tool,
        operation->package.host_platform, operation->package.target_platform,
        operation->package.version, operation->format);
    if (err != CUP_OK) {
        return err;
    }
    err = manifest_build_checksum_url(&operation->context.manifest,
        operation->checksum_url, sizeof(operation->checksum_url),
        operation->package.component, operation->package.tool,
        operation->package.host_platform, operation->package.target_platform,
        operation->package.version);
    if (err != CUP_OK) {
        return err;
    }

    err = fetch_package(operation->archive_path,
        sizeof(operation->archive_path), operation->url,
        operation->checksum_url, &operation->package, operation->format,
        FETCH_ALLOW_CACHE, &source);
    if (err != CUP_OK) {
        return err;
    }

    if (interrupt_requested()) {
        return CUP_ERR_INTERRUPT;
    }

    if (source == FETCH_SOURCE_CACHE) {
        printf("==> Using cached package archive.\n");
    } else {
        printf("==> Downloaded package archive.\n");
    }

    err = extract_and_validate_package(operation);
    if (err != CUP_OK && source == FETCH_SOURCE_CACHE &&
        package_failure_allows_refresh(err)) {
        printf("==> Cached package is invalid; downloading it again...\n");

        err = fetch_discard_cached_package(operation->archive_path);
        if (err != CUP_OK) {
            return err;
        }

        err = reset_install_staging(operation);
        if (err != CUP_OK) {
            return err;
        }

        err = fetch_package(operation->archive_path,
            sizeof(operation->archive_path), operation->url,
            operation->checksum_url, &operation->package, operation->format,
            FETCH_REFRESH_CACHE, &source);
        if (err != CUP_OK) {
            return err;
        }

        printf("==> Downloaded replacement package archive.\n");
        err = extract_and_validate_package(operation);
    }

    if (err != CUP_OK) {
        if (source == FETCH_SOURCE_NETWORK &&
            package_failure_allows_refresh(err)) {
            return discard_invalid_cache(operation, err);
        }
        return err;
    }

    err = package_set_info_read_only(operation->staging_path);
    if (err != CUP_OK) {
        return err;
    }

    return layout_ensure_package_parent(&operation->package);
}

static CupError prepare_default_change(InstallOperation *operation,
    CupState *candidate, int package_is_new) {
    const char *current_default;
    int should_set_default = 0;
    CupError err;

    current_default = state_get_default(candidate,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform);

    if (current_default == NULL && package_is_new) {
        should_set_default = 1;
        operation->made_default = 1;
    } else if (operation->kind == INSTALL_REQUEST_UPDATE &&
        operation->expected_default[0] != '\0' &&
        current_default != NULL &&
        strcmp(current_default, operation->expected_default) == 0 &&
        strcmp(current_default, operation->request.resolved_entry) != 0) {
        should_set_default = 1;
        operation->default_moved = 1;
    }

    if (!should_set_default) {
        return CUP_OK;
    }

    err = state_set_default(candidate, operation->package.component,
        operation->package.host_platform, operation->package.target_platform,
        operation->request.resolved_entry);
    if (err != CUP_OK) {
        return err;
    }

    err = entrypoint_plan_build(&operation->entrypoints, candidate);
    if (err != CUP_OK) {
        return err;
    }
    operation->entrypoints_ready = 1;
    return CUP_OK;
}

static CupError save_default_change(InstallOperation *operation,
    const CupState *candidate) {
    CupError err;

    if (!operation->entrypoints_ready) {
        return CUP_OK;
    }

    operation->context.state = *candidate;
    err = state_save(&operation->context.state);
    if (err != CUP_OK) {
        return err;
    }

    err = entrypoint_plan_apply(&operation->entrypoints);
    if (err != CUP_OK) {
        fprintf(stderr,
            "Error: the updated default was saved, but its entry points "
            "could not be rebuilt. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

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

    err = system_move_path(operation->staging_path, operation->install_path,
        &commit_state);
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
    err = state_add_installed(&candidate,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform,
        operation->request.resolved_entry);
    if (err != CUP_OK) {
        return err;
    }

    err = prepare_default_change(operation, &candidate, 1);
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

    err = transaction_clear();
    if (err != CUP_OK) {
        fprintf(stderr,
            "Warning: installation committed, but transaction cleanup failed. "
            "Run 'cup repair'.\n");
        cleanup_failed = 1;
    } else {
        operation->journal_started = 0;
    }

    if (operation->entrypoints_ready) {
        err = entrypoint_plan_apply(&operation->entrypoints);
        if (err != CUP_OK) {
            fprintf(stderr,
                "Error: installation and its default were saved, but entry "
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
        err = system_move_path(operation->install_path,
            operation->staging_path, &commit_state);
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
        err = transaction_clear();
        if (err != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        operation->journal_started = 0;
    }

    return CUP_OK;
}

static void print_install_result(const InstallOperation *operation) {
    printf("Installed %s ", operation->package.component);
    entry_request_print(stdout, &operation->request);
    printf(" for host '%s', target '%s'%s.\n",
        operation->package.host_platform, operation->package.target_platform,
        operation->made_default ? " and set it as the first default" : "");
}

static CupError run_install(InstallOperation *operation,
    const char *component, const char *entry, const char *target_override,
    const char *format_override, InstallRequestKind kind,
    const char *expected_default) {
    CupError err;

    interrupt_setup();

    err = prepare_install(operation, component, entry, target_override,
        format_override, kind, expected_default);
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

    if (err != CUP_OK && err != CUP_ERR_COMMIT &&
        operation->staging_created) {
        if (rollback_install(operation) != CUP_OK) {
            fprintf(stderr,
                "Error: installation failed and rollback could not be "
                "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
    }

done:
    command_context_end(&operation->context);
    interrupt_reset();
    return err;
}

CupError command_install(const char *component, const char *entry,
    const char *target_override, const char *format_override) {
    InstallOperation operation = {0};
    CupError err;

    entrypoint_plan_init(&operation.entrypoints);
    err = run_install(&operation, component, entry, target_override,
        format_override, INSTALL_REQUEST_USER, NULL);
    if (err == CUP_OK) {
        print_install_result(&operation);
    }
    entrypoint_plan_free(&operation.entrypoints);
    return err;
}

CupError command_update_scope(const char *component, const char *tool,
    const char *target_override, const char *expected_default,
    int *installed, int *default_moved) {
    InstallOperation operation = {0};
    CupError err;
    char entry[MAX_ENTRY_LEN];

    if (installed == NULL || default_moved == NULL ||
        text_is_empty(component) || text_is_empty(tool)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *installed = 0;
    *default_moved = 0;

    err = entry_build(entry, sizeof(entry), tool, "stable");
    if (err != CUP_OK) {
        return err;
    }

    entrypoint_plan_init(&operation.entrypoints);
    err = run_install(&operation, component, entry, target_override, NULL,
        INSTALL_REQUEST_UPDATE, expected_default);
    if (err == CUP_OK) {
        *installed = !operation.package_already_installed;
        *default_moved = operation.default_moved || operation.made_default;
    }
    entrypoint_plan_free(&operation.entrypoints);
    return err;
}
