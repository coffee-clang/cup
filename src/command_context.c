/*
 * Owns the shared lifetime of one CLI command: canonical root validation, platform resolution,
 * lock acquisition, state loading and catalog loading.
 */

#include "command_context.h"

#include "cup_assets.h"
#include "package_selector.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Command context initialization and lifetime. */
static CupError resolve_platforms(CommandContext *context, const char *target_override) {
    CupError err;

    err = platform_get_host(context->host_platform, sizeof(context->host_platform));
    if (err != CUP_OK) {
        return err;
    }

    if (text_is_empty(target_override)) {
        return text_copy(
            context->target_platform, sizeof(context->target_platform), context->host_platform);
    }

    err = text_copy_lower_ascii(
        context->target_platform, sizeof(context->target_platform), target_override);
    if (err != CUP_OK) {
        return err;
    }
    return platform_validate(context->target_platform);
}

static CupError validate_cup_assets(void) {
    CupAssetsInspection inspection;
    CupError err = cup_assets_inspect(&inspection);

    if (err != CUP_OK) {
        return err;
    }
    if (cup_assets_installed_is_valid(&inspection) ||
        cup_assets_development_is_valid(&inspection)) {
        return CUP_OK;
    }
    return CUP_ERR_VALIDATION;
}

static CupError reject_pending_uninstall(void) {
    CupError err;
    int pending;

    err = cup_assets_uninstall_is_pending(&pending);
    if (err != CUP_OK) {
        return err;
    }
    if (!pending) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: cup uninstall is in progress or did not finish. "
            "Run the installer again if the marker is stale.\n");
    return CUP_ERR_LOCK;
}

/* Mutable runtime initialization. Directory creation and asset validation happen only after the
 * caller has selected a mutating context. */
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

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    LayoutRuntimeStatus runtime_status;
    SystemLockMode lock_mode = mode;
    CupError err;
    char root[MAX_PATH_LEN];
    char lock_path[MAX_PATH_LEN];

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(context, 0, sizeof(*context));
    package_catalog_init(&context->catalog);

    err = reject_pending_uninstall();
    if (err != CUP_OK) {
        return err;
    }

    err = resolve_platforms(context, target_override);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        return err;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr,
                "Error: cup runtime structure is incomplete. "
                "Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_FILESYSTEM;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        err = validate_cup_assets();
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Error: CUP assets are unavailable. "
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

    /*
     * The marker can appear while this command is waiting for the lock.
     * Recheck it while holding the lock so a command cannot start after
     * uninstall has committed to deferred runtime removal.
     */
    err = reject_pending_uninstall();
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr,
                "Error: cup runtime structure is incomplete. "
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

    context->runtime_available = 1;
    return CUP_OK;
}

/* Read-only context. Missing roots are treated as an uninitialized installation and are never
 * created as a side effect. */
CupError command_context_begin_read_only(CommandContext *context, const char *target_override) {
    LayoutRuntimeStatus runtime_status;
    CupError err;
    char lock_path[MAX_PATH_LEN];

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(context, 0, sizeof(*context));
    package_catalog_init(&context->catalog);

    err = resolve_platforms(context, target_override);
    if (err != CUP_OK) {
        return err;
    }
    err = reject_pending_uninstall();
    if (err != CUP_OK) {
        return err;
    }
    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        return err;
    }
    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        context->runtime_available = 0;
        return CUP_OK;
    }
    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr,
                "Error: cup runtime structure is incomplete. "
                "Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_FILESYSTEM;
    }

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) {
        err = system_lock_acquire(&context->lock, lock_path, SYSTEM_LOCK_SHARED);
    }
    if (err != CUP_OK) {
        if (err == CUP_ERR_LOCK) {
            fprintf(stderr, "Error: another cup operation is currently running.\n");
        }
        return err;
    }
    err = reject_pending_uninstall();
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    err = layout_get_runtime_status(&runtime_status);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }
    if (runtime_status == LAYOUT_RUNTIME_INCOMPLETE) {
        fprintf(stderr,
                "Error: cup runtime structure is incomplete. "
                "Run 'cup doctor' and 'cup repair'.\n");
        command_context_end(context);
        return CUP_ERR_FILESYSTEM;
    }
    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        command_context_end(context);
        context->runtime_available = 0;
        err = resolve_platforms(context, target_override);
        return err;
    }

    context->runtime_available = 1;
    return CUP_OK;
}

void command_context_end(CommandContext *context) {
    if (context == NULL) {
        return;
    }

    package_catalog_free(&context->catalog);
    system_lock_release(&context->lock);
    memset(context, 0, sizeof(*context));
}

/* Lazy model loading. State and catalog errors remain separate so query commands can produce
 * precise degraded output. */
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
    err = state_validate_current_host(&context->state, context->host_platform);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }

    return CUP_OK;
}

CupError command_context_load_catalog(CommandContext *context) {
    CupError err;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    context->has_catalog = 0;
    err = package_catalog_load(&context->catalog);
    if (err != CUP_OK) {
        return err;
    }

    context->has_catalog = 1;
    return CUP_OK;
}

void command_context_try_catalog(CommandContext *context) {
    if (context == NULL) {
        return;
    }

    context->has_catalog = 0;
    if (package_catalog_load(&context->catalog) == CUP_OK) {
        context->has_catalog = 1;
    }
}
