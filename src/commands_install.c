#include "commands.h"

#include "command_context.h"
#include "extract.h"
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

typedef struct {
    CommandContext context;
    EntryRequest request;
    PackageIdentity package;
    char format[MAX_NAME_LEN];
    char url[MAX_MANIFEST_URL_LEN];
    char archive_path[MAX_PATH_LEN];
    char staging_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int staging_created;
    int package_moved;
    int journal_started;
    int made_default;
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

    err = text_format(format, size, "%s", format_override);
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
    const char *format_override) {
    CupError err;
    int version_available;

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

    err = resolve_archive_format(&operation->context.manifest,
        &operation->package, format_override, operation->format,
        sizeof(operation->format));
    if (err != CUP_OK) {
        return err;
    }

    err = command_require_absent(&operation->context, &operation->package);
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

    err = fetch_package(operation->archive_path,
        sizeof(operation->archive_path), operation->url,
        &operation->package, operation->format,
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
            &operation->package, operation->format,
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

static CupError commit_install(InstallOperation *operation) {
    CupError err;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    int had_default;

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

    had_default = state_find_default(&operation->context.state,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform) != -1;

    err = state_add_installed(&operation->context.state,
        operation->package.component, operation->package.host_platform,
        operation->package.target_platform,
        operation->request.resolved_entry);
    if (err != CUP_OK) {
        return err;
    }

    if (!had_default) {
        err = state_set_default(&operation->context.state,
            operation->package.component, operation->package.host_platform,
            operation->package.target_platform,
            operation->request.resolved_entry);
        if (err != CUP_OK) {
            state_remove_installed(&operation->context.state,
                operation->package.component,
                operation->package.host_platform,
                operation->package.target_platform,
                operation->request.resolved_entry);
            return err;
        }
        operation->made_default = 1;

        err = entrypoints_validate(&operation->context.state);
        if (err != CUP_OK) {
            state_clear_matching_default(&operation->context.state,
                operation->package.component,
                operation->package.host_platform,
                operation->package.target_platform,
                operation->request.resolved_entry);
            state_remove_installed(&operation->context.state,
                operation->package.component,
                operation->package.host_platform,
                operation->package.target_platform,
                operation->request.resolved_entry);
            operation->made_default = 0;
            return err;
        }
    }

    err = state_save(&operation->context.state);
    if (err != CUP_OK) {
        if (err == CUP_ERR_COMMIT) {
            fprintf(stderr,
                "Error: installation state was applied, but its durability "
                "could not be confirmed. Run 'cup repair'.\n");
            return err;
        }

        if (operation->made_default) {
            state_clear_matching_default(&operation->context.state,
                operation->package.component,
                operation->package.host_platform,
                operation->package.target_platform,
                operation->request.resolved_entry);
            operation->made_default = 0;
        }
        if (state_remove_installed(&operation->context.state,
            operation->package.component, operation->package.host_platform,
            operation->package.target_platform,
            operation->request.resolved_entry) != CUP_OK) {
            return CUP_ERR_ROLLBACK;
        }
        return err;
    }

    err = transaction_clear();
    if (err != CUP_OK) {
        fprintf(stderr,
            "Warning: installation committed, but transaction cleanup failed. "
            "Run 'cup repair'.\n");
    } else {
        operation->journal_started = 0;
    }

    if (operation->made_default) {
        err = entrypoints_sync(&operation->context.state);
        if (err != CUP_OK) {
            fprintf(stderr,
                "Error: installation and its automatic default were saved, "
                "but entry points could not be rebuilt. Run 'cup repair'.\n");
            return CUP_ERR_COMMIT;
        }
    }

    return CUP_OK;
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

CupError command_install(const char *component, const char *entry,
    const char *target_override, const char *format_override) {
    InstallOperation operation = {0};
    CupError err;

    interrupt_setup();

    err = prepare_install(&operation, component, entry,
        target_override, format_override);
    if (err != CUP_OK) {
        if (operation.staging_created && err != CUP_ERR_COMMIT &&
            rollback_install(&operation) != CUP_OK) {
            fprintf(stderr,
                "Error: installation failed and rollback could not be "
                "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
        goto done;
    }

    err = extract_install_package(&operation);
    if (err == CUP_OK) {
        err = commit_install(&operation);
    }

    if (err != CUP_OK && err != CUP_ERR_COMMIT) {
        if (rollback_install(&operation) != CUP_OK) {
            fprintf(stderr,
                "Error: installation failed and rollback could not be "
                "completed. Run 'cup repair'.\n");
            err = CUP_ERR_ROLLBACK;
        }
        goto done;
    }

    if (err == CUP_OK) {
        print_install_result(&operation);
    }

done:
    command_context_end(&operation.context);
    interrupt_reset();
    return err;
}
