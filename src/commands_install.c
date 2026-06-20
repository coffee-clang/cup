#include "commands.h"

#include "command_context.h"
#include "extract.h"
#include "fetch.h"
#include "filesystem.h"
#include "interrupt.h"
#include "layout.h"
#include "manifest.h"
#include "package.h"
#include "state.h"
#include "system.h"
#include "transaction.h"
#include "util.h"

#include <stdio.h>

// INSTALL HELPERS
static CupError resolve_format(const Manifest *manifest, const PackageIdentity *package,
    const char *override, char *format, size_t size) {
    CupError err;
    int supported;

    if (is_empty_string(override)) {
        return manifest_get_default_format(manifest, format, size,
            package->component, package->tool,
            package->host_platform, package->target_platform);
    }

    err = checked_snprintf(format, size, "%s", override);
    if (err != CUP_OK) {
        return err;
    }

    err = manifest_has_format(manifest, package->component, package->tool,
        package->host_platform, package->target_platform, format, &supported);
    if (err != CUP_OK) {
        return err;
    }

    if (!supported) {
        fprintf(stderr, "Error: archive format '%s' is not available for '%s'.\n",
            format, package->tool);
        return CUP_ERR_NOT_AVAILABLE;
    }

    return CUP_OK;
}

static CupError rollback_install(const char *tmp_path, const char *install_path,
    int package_moved, int journal_started) {
    if (package_moved && system_move_path(install_path, tmp_path) != CUP_OK) {
        return CUP_ERR_ROLLBACK;
    }

    if (filesystem_remove_tree(tmp_path) != CUP_OK) {
        return CUP_ERR_ROLLBACK;
    }

    if (journal_started && transaction_clear() != CUP_OK) {
        return CUP_ERR_ROLLBACK;
    }

    return CUP_OK;
}

// INSTALL COMMAND
CupError handle_install(const char *component, const char *entry,
    const char *target_override, const char *format_override) {
    CommandContext context = {0};
    EntryRequest request;
    PackageIdentity package;
    CupError err;
    CupError rollback_err;
    char format[MAX_NAME_LEN];
    char url[MAX_MANIFEST_URL_LEN];
    char archive[MAX_PATH_LEN];
    char tmp_path[MAX_PATH_LEN];
    char install_path[MAX_PATH_LEN];
    int available;
    int package_moved = 0;
    int journal_started = 0;

    interrupt_setup();

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

    err = command_context_load_manifest(&context);
    if (err != CUP_OK) {
        goto done;
    }

    err = entry_request_resolve(&context.manifest, component,
        context.host_platform, context.target_platform, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package, component, request.tool,
        context.host_platform, context.target_platform, request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = manifest_has_version(&context.manifest, component, package.tool,
        package.host_platform, package.target_platform, package.version, &available);
    if (err != CUP_OK) {
        goto done;
    }

    if (!available) {
        fprintf(stderr, "Error: version '%s' is not available for '%s' on host '%s', target '%s'.\n",
            package.version, package.tool,
            package.host_platform, package.target_platform);
        err = CUP_ERR_NOT_AVAILABLE;
        goto done;
    }

    err = resolve_format(&context.manifest, &package,
        format_override, format, sizeof(format));
    if (err != CUP_OK) {
        goto done;
    }

    err = command_require_absent(&context, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_create_tmp_dir(tmp_path, sizeof(tmp_path), "install", &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), &package);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = transaction_begin(TRANSACTION_INSTALL, &package, tmp_path);
    if (err != CUP_OK) {
        goto rollback;
    }
    journal_started = 1;

    printf("==> Downloading %s@%s...\n", package.tool, package.version);

    err = manifest_build_url(&context.manifest, url, sizeof(url),
        package.component, package.tool, package.host_platform,
        package.target_platform, package.version, format);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = fetch_package(archive, sizeof(archive), url, &package, format, 0);
    if (err != CUP_OK) {
        goto rollback;
    }

    if (interrupt_requested()) {
        err = CUP_ERR_INTERRUPT;
        goto rollback;
    }

    printf("==> Extracting package...\n");

    err = extract_archive(archive, tmp_path);
    if (err == CUP_ERR_ARCHIVE) {
        printf("==> Cached archive is invalid; downloading it again...\n");

        filesystem_remove_tree(tmp_path);
        err = filesystem_ensure_directory(tmp_path);
        if (err != CUP_OK) {
            goto rollback;
        }

        err = fetch_package(archive, sizeof(archive), url, &package, format, 1);
        if (err != CUP_OK) {
            goto rollback;
        }

        err = extract_archive(archive, tmp_path);
    }

    if (err != CUP_OK) {
        goto rollback;
    }

    if (interrupt_requested()) {
        err = CUP_ERR_INTERRUPT;
        goto rollback;
    }

    printf("==> Validating package...\n");

    err = package_validate(tmp_path, &package);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = package_set_info_read_only(tmp_path);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = layout_ensure_package_parent(&package);
    if (err != CUP_OK) {
        goto rollback;
    }

    printf("==> Committing installation...\n");

    err = system_move_path(tmp_path, install_path);
    if (err != CUP_OK) {
        goto rollback;
    }
    package_moved = 1;

    err = state_add_installed(&context.state, component,
        package.host_platform, package.target_platform, request.resolved_entry);
    if (err != CUP_OK) {
        goto rollback;
    }

    err = state_save(&context.state);
    if (err != CUP_OK) {
        state_remove_installed(&context.state, component,
            package.host_platform, package.target_platform, request.resolved_entry);
        goto rollback;
    }

    err = transaction_clear();
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: installation committed, but transaction cleanup failed. "
            "Run 'cup repair'.\n");
        err = CUP_OK;
    }

    printf("Installed %s ", component);
    entry_request_print(stdout, &request);
    printf(" for host '%s', target '%s'.\n",
        package.host_platform, package.target_platform);
    goto done;

rollback:
    rollback_err = rollback_install(tmp_path, install_path,
        package_moved, journal_started);
    if (rollback_err != CUP_OK) {
        fprintf(stderr, "Error: installation failed and rollback could not be completed. "
            "Run 'cup repair'.\n");
        err = CUP_ERR_ROLLBACK;
    }

done:
    command_context_end(&context);
    interrupt_reset();
    return err;
}
