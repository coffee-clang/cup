#include "command_context.h"

#include "entry.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "transaction.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup-windows.ps1"
#else
#define DEVELOPMENT_UNINSTALL_PATH "scripts/install/uninstall-cup.sh"
#endif

// COMMAND LIFECYCLE
static CupError resolve_platforms(CommandContext *context, const char *target_override) {
    CupError err;

    err = get_host_platform(context->host_platform, sizeof(context->host_platform));
    if (err != CUP_OK) {
        return err;
    }

    if (is_empty_string(target_override)) {
        return checked_snprintf(context->target_platform, sizeof(context->target_platform),
            "%s", context->host_platform);
    }

    err = validate_platform(target_override);
    if (err != CUP_OK) {
        return err;
    }

    return checked_snprintf(context->target_platform, sizeof(context->target_platform),
        "%s", target_override);
}

static CupError validate_bootstrap(void) {
    Manifest manifest;
    CupError err;
    char uninstall[MAX_PATH_LEN];
    int executable;
    int is_file;

    manifest_init(&manifest);
    err = manifest_load(&manifest);
    manifest_free(&manifest);
    if (err != CUP_OK) {
        return err;
    }

    if (layout_get_uninstall_path(uninstall, sizeof(uninstall)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }

    err = system_is_regular_file(uninstall, &is_file);
    if (err != CUP_OK) {
        return err;
    }

    if (!is_file) {
        err = system_is_regular_file(DEVELOPMENT_UNINSTALL_PATH, &is_file);
        if (err != CUP_OK || !is_file) {
            return CUP_ERR_FILESYSTEM;
        }

        err = system_is_executable(DEVELOPMENT_UNINSTALL_PATH, &executable);
    } else {
        err = system_is_executable(uninstall, &executable);
    }

    if (err != CUP_OK || !executable) {
        return CUP_ERR_FILESYSTEM;
    }

    return CUP_OK;
}

static CupError initialize_runtime(void) {
    CupState state;
    CupError err;

    err = layout_ensure_runtime();
    if (err != CUP_OK) {
        return err;
    }

    memset(&state, 0, sizeof(state));
    return state_save(&state);
}

CupError command_context_begin(CommandContext *context, const char *target_override, SystemLockMode mode) {
    LayoutRuntimeStatus runtime_status;
    SystemLockMode lock_mode = mode;
    CupError err;
    char root[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(context, 0, sizeof(*context));
    manifest_init(&context->manifest);

    err = resolve_platforms(context, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        return err;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr, "Error: cup runtime structure is incomplete. "
            "Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_FILESYSTEM;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        err = validate_bootstrap();
        if (err != CUP_OK) {
            fprintf(stderr, "Error: cup bootstrap files are unavailable. "
                "Run the installer or execute cup from the repository root.\n");
            return err;
        }

        err = layout_ensure_root();
        if (err != CUP_OK) {
            return err;
        }

        lock_mode = SYSTEM_LOCK_EXCLUSIVE;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err != CUP_OK) {
        return err;
    }

    err = system_lock_acquire(&context->lock, lock_path, lock_mode);
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr, "Error: cup runtime structure is incomplete. "
            "Run 'cup doctor' and 'cup repair'.\n");
        command_context_end(context);
        return CUP_ERR_FILESYSTEM;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        err = initialize_runtime();
        if (err != CUP_OK) {
            command_context_end(context);
            return err;
        }

        if (layout_get_root(root, sizeof(root)) == CUP_OK) {
            printf("Initialized cup runtime at '%s'.\n", root);
        }
    }

    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    if (context == NULL) {
        return;
    }

    manifest_free(&context->manifest);
    system_lock_release(&context->lock);
    memset(context, 0, sizeof(*context));
}

CupError command_context_load_state(CommandContext *context) {
    StateFileStatus status;
    CupError err;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = state_load(&context->state, &status);
    if (err != CUP_OK) {
        return err;
    }

    if (status == STATE_FILE_MISSING) {
        fprintf(stderr, "Error: state.txt is missing. Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = state_validate(&context->state);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

CupError command_context_load_manifest(CommandContext *context) {
    CupError err;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = manifest_load(&context->manifest);
    if (err != CUP_OK) {
        return err;
    }

    context->has_manifest = 1;
    return CUP_OK;
}

void command_context_try_manifest(CommandContext *context) {
    if (context == NULL) {
        return;
    }

    if (manifest_load(&context->manifest) == CUP_OK) {
        context->has_manifest = 1;
    }
}

CupError command_require_no_transaction(void) {
    Transaction transaction;
    TransactionFileStatus status;
    CupError err;

    transaction_init(&transaction);
    err = transaction_load(&transaction, &status);
    if (err != CUP_OK) {
        fprintf(stderr, "Error: transaction journal is invalid. Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_TRANSACTION;
    }

    if (status == TRANSACTION_FILE_LOADED) {
        fprintf(stderr, "Error: an interrupted %s transaction for '%s@%s' must be repaired first.\n",
            transaction_operation_name(transaction.operation), transaction.package.tool,
            transaction.package.version);
        return CUP_ERR_TRANSACTION;
    }

    return CUP_OK;
}

// ENTRY REQUESTS
CupError entry_request_parse(const char *component, const char *entry, EntryRequest *request) {
    CupError err;

    if (request == NULL || is_empty_string(component) || is_empty_string(entry)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(request, 0, sizeof(*request));

    err = validate_component(component);
    if (err != CUP_OK) {
        return err;
    }

    err = checked_snprintf(request->input_entry, sizeof(request->input_entry), "%s", entry);
    if (err != CUP_OK || entry_parse(entry, request->tool, sizeof(request->tool),
        request->release, sizeof(request->release)) != CUP_OK) {
        fprintf(stderr, "Error: invalid entry '%s'. Expected '<tool>@<release>'.\n", entry);
        return CUP_ERR_INVALID_INPUT;
    }

    err = validate_tool_for_component(component, request->tool);
    if (err != CUP_OK) {
        return err;
    }

    if (!entry_is_stable(request->release) && !path_is_safe_identifier(request->release)) {
        fprintf(stderr, "Error: invalid release identifier '%s'.\n", request->release);
        return CUP_ERR_INVALID_RELEASE;
    }

    return CUP_OK;
}

CupError entry_request_resolve(const Manifest *manifest, const char *component,
    const char *host_platform, const char *target_platform, EntryRequest *request) {
    CupError err;

    if (request == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (entry_is_stable(request->release)) {
        if (manifest == NULL) {
            return CUP_ERR_MANIFEST;
        }

        err = manifest_resolve_stable(manifest, request->resolved_release,
            sizeof(request->resolved_release), component, request->tool,
            host_platform, target_platform);
        if (err != CUP_OK) {
            return err;
        }
    } else {
        err = checked_snprintf(request->resolved_release, sizeof(request->resolved_release),
            "%s", request->release);
        if (err != CUP_OK) {
            return err;
        }
    }

    if (!path_is_safe_identifier(request->resolved_release)) {
        return CUP_ERR_INVALID_RELEASE;
    }

    return entry_build(request->resolved_entry, sizeof(request->resolved_entry),
        request->tool, request->resolved_release);
}

void entry_request_print(FILE *stream, const EntryRequest *request) {
    if (stream == NULL || request == NULL) {
        return;
    }

    if (strcmp(request->input_entry, request->resolved_entry) == 0) {
        fprintf(stream, "%s", request->input_entry);
        return;
    }

    fprintf(stream, "%s -> %s", request->input_entry, request->resolved_entry);
}

// PACKAGE PRECONDITIONS
static CupError get_package_presence(const CommandContext *context, const PackageIdentity *package,
    char *entry, size_t entry_size, int *in_state, int *on_disk) {
    CupError err;

    if (context == NULL || package == NULL || entry == NULL || entry_size == 0 ||
        in_state == NULL || on_disk == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = entry_build(entry, entry_size, package->tool, package->version);
    if (err != CUP_OK) {
        return err;
    }

    *in_state = state_find_installed(&context->state, package->component,
        package->host_platform, package->target_platform, entry) != -1;

    return package_exists(package, on_disk);
}

CupError command_require_installed(const CommandContext *context, const PackageIdentity *package) {
    CupError err;
    char entry[MAX_ENTRY_LEN];
    int in_state;
    int on_disk;

    err = get_package_presence(context, package, entry, sizeof(entry), &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (in_state && on_disk) {
        return CUP_OK;
    }

    if (!in_state && !on_disk) {
        fprintf(stderr, "Error: package '%s:%s' is not installed for host '%s', target '%s'.\n",
            package->component, entry, package->host_platform, package->target_platform);
        return CUP_ERR_NOT_INSTALLED;
    }

    fprintf(stderr, "Error: package '%s:%s' is inconsistent between state and components. "
        "Run 'cup doctor' and 'cup repair'.\n", package->component, entry);
    return CUP_ERR_INCONSISTENT_STATE;
}

CupError command_require_absent(const CommandContext *context, const PackageIdentity *package) {
    CupError err;
    char entry[MAX_ENTRY_LEN];
    int in_state;
    int on_disk;

    err = get_package_presence(context, package, entry, sizeof(entry), &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }

    if (!in_state && !on_disk) {
        return CUP_OK;
    }

    if (in_state && on_disk) {
        fprintf(stderr, "Error: package '%s:%s' is already installed for host '%s', target '%s'.\n",
            package->component, entry, package->host_platform, package->target_platform);
        return CUP_ERR_ALREADY_INSTALLED;
    }

    fprintf(stderr, "Error: package '%s:%s' is inconsistent between state and components. "
        "Run 'cup doctor' and 'cup repair'.\n", package->component, entry);
    return CUP_ERR_INCONSISTENT_STATE;
}
