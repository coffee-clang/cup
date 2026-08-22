/*
 * Owns the shared lifetime of one CLI command: selected-root validation, platform resolution,
 * lock acquisition, state loading and catalog loading.
 */

#include "command_context.h"

#include "assets.h"
#include "package_selector.h"
#include "layout.h"
#include "interrupt.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "runtime_journal.h"
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

static CupError prepare_context(CommandContext *context, const char *target_override) {
    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(context, 0, sizeof(*context));
    package_catalog_init(&context->catalog);
    return resolve_platforms(context, target_override);
}

static CupError validate_assets(void) {
    AssetsInspection inspection;
    CupError err = assets_inspect(&inspection);

    if (err != CUP_OK) {
        return err;
    }
    if (assets_installed_is_valid(&inspection) ||
        assets_development_is_valid(&inspection)) {
        return CUP_OK;
    }
    return CUP_ERR_VALIDATION;
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
    return state_save(&state, NULL, NULL);
}

static CupError require_usable_runtime(LayoutRuntimeStatus status) {
    if (status != LAYOUT_RUNTIME_INCOMPLETE) {
        return CUP_OK;
    }

    fprintf(stderr,
            "Error: cup runtime structure is incomplete. "
            "Run 'cup doctor' and 'cup repair'.\n");
    return CUP_ERR_FILESYSTEM;
}

static CupError acquire_runtime_lock(CommandContext *context, SystemLockMode mode) {
    CupError err;
    char lock_path[MAX_PATH_LEN];

    err = layout_get_lock_path(lock_path, sizeof(lock_path));
    if (err == CUP_OK) {
        err = system_lock_acquire(&context->lock, lock_path, mode);
    }
    if (err == CUP_ERR_LOCK) {
        fprintf(stderr, "Error: another cup operation is currently running.\n");
    }
    return err;
}

/* Validate every runtime precondition only after the final requested lock is held. */
static CupError inspect_locked_runtime(LayoutRuntimeStatus *status) {
    CupError err = layout_root_snapshot_validate();

    if (err == CUP_OK) {
        err = runtime_journal_require_none();
    }
    if (err == CUP_OK) {
        err = layout_get_runtime_status(status);
    }
    if (err == CUP_OK) {
        err = require_usable_runtime(*status);
    }
    return err;
}

/* A missing root cannot contain cup.lock. Create only that absent bootstrap root, then retry the
 * canonical exclusive lock. Existing roots are always locked before marker, mode or runtime
 * repair. */
static CupError acquire_bootstrap_lock(CommandContext *context) {
    CupError err = acquire_runtime_lock(context, SYSTEM_LOCK_EXCLUSIVE);
    SystemPathKind root_kind;
    char root_path[MAX_PATH_LEN];

    if (err != CUP_ERR_FILESYSTEM) {
        return err;
    }

    /* A filesystem error is not proof that the root is absent: cup.lock may be the wrong kind or
     * an existing root may be inaccessible. Only a no-follow missing-root classification permits
     * creation before the canonical lock exists. */
    err = layout_get_root(root_path, sizeof(root_path));
    if (err == CUP_OK) {
        err = system_get_path_kind(root_path, &root_kind);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (root_kind != SYSTEM_PATH_MISSING) {
        return CUP_ERR_FILESYSTEM;
    }

    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = layout_ensure_root();
    }
    if (err != CUP_OK) {
        return err;
    }
    return acquire_runtime_lock(context, SYSTEM_LOCK_EXCLUSIVE);
}

static CupError initialize_locked_runtime(LayoutRuntimeStatus runtime_status) {
    CupError err;
    char root[MAX_PATH_LEN];

    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = layout_ensure_root();
    }
    if (err != CUP_OK) {
        return err;
    }
    if (runtime_status != LAYOUT_RUNTIME_MISSING) {
        return CUP_OK;
    }

    err = interrupt_safe_point();
    if (err == CUP_OK) {
        err = initialize_runtime();
    }
    if (err != CUP_OK) {
        return err;
    }
    if (layout_get_root(root, sizeof(root)) == CUP_OK) {
        printf("Initialized cup runtime at '%s'.\n", root);
    }
    return CUP_OK;
}

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    LayoutRuntimeStatus runtime_status = LAYOUT_RUNTIME_MISSING;
    SystemLockMode lock_mode = mode;
    CupError err;

    if (context == NULL || (mode != SYSTEM_LOCK_SHARED && mode != SYSTEM_LOCK_EXCLUSIVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = prepare_context(context, target_override);
    if (err == CUP_OK) {
        err = layout_get_runtime_status(&runtime_status);
    }
    if (err != CUP_OK) {
        return err;
    }

    /* The pre-lock status selects only the lock/bootstrap route. It is never authoritative for
     * readiness: a concurrent repair may complete, or the runtime may degrade, before ownership is
     * acquired. inspect_locked_runtime() makes the only usable/incomplete decision. */
    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        err = validate_assets();
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Error: cup assets are unavailable. "
                    "Run the installer or execute cup from the repository root.\n");
            return err;
        }
        lock_mode = SYSTEM_LOCK_EXCLUSIVE;
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = acquire_bootstrap_lock(context);
        }
    } else {
        err = acquire_runtime_lock(context, lock_mode);
    }
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_locked_runtime(&runtime_status);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    /* A shared preflight never initializes under a shared lock. Reacquire the exclusive lock and
     * rebuild the entire locked snapshot when the runtime disappeared concurrently. */
    if (runtime_status == LAYOUT_RUNTIME_MISSING && lock_mode == SYSTEM_LOCK_SHARED) {
        command_context_end(context);
        err = prepare_context(context, target_override);
        if (err != CUP_OK) {
            return err;
        }
        err = validate_assets();
        if (err != CUP_OK) {
            return err;
        }
        lock_mode = SYSTEM_LOCK_EXCLUSIVE;
        err = interrupt_safe_point();
        if (err == CUP_OK) {
            err = acquire_bootstrap_lock(context);
        }
        if (err != CUP_OK) {
            return err;
        }
        err = inspect_locked_runtime(&runtime_status);
        if (err != CUP_OK) {
            command_context_end(context);
            return err;
        }
    }

    if (lock_mode == SYSTEM_LOCK_EXCLUSIVE) {
        /* The pre-lock asset check only avoids creating an unusable bootstrap root. Installed or
         * development assets are revalidated under the final exclusive snapshot before they can
         * authorize runtime initialization. */
        if (runtime_status == LAYOUT_RUNTIME_MISSING) {
            err = validate_assets();
        }
        if (err == CUP_OK) {
            err = initialize_locked_runtime(runtime_status);
        }
        if (err != CUP_OK) {
            command_context_end(context);
            return err;
        }
    }

    context->runtime_available = 1;
    return CUP_OK;
}

static CupError selected_root_is_missing(int *missing) {
    SystemPathKind kind;
    CupError err;
    char root[MAX_PATH_LEN];

    if (missing == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *missing = 0;
    err = layout_get_root(root, sizeof(root));
    if (err == CUP_OK) {
        err = system_get_path_kind(root, &kind);
    }
    if (err != CUP_OK) {
        return err;
    }
    if (kind == SYSTEM_PATH_MISSING) {
        *missing = 1;
        return CUP_OK;
    }
    return kind == SYSTEM_PATH_DIRECTORY ? CUP_OK : CUP_ERR_FILESYSTEM;
}

/* Read-only context. Missing roots are treated as an uninitialized installation and are never
 * created as a side effect. */
CupError command_context_begin_read_only(CommandContext *context, const char *target_override) {
    LayoutRuntimeStatus runtime_status = LAYOUT_RUNTIME_MISSING;
    CupError err;

    if (context == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = prepare_context(context, target_override);
    if (err == CUP_OK) {
        err = layout_get_runtime_status(&runtime_status);
    }
    if (err != CUP_OK) {
        return err;
    }
    /* As in mutating contexts, the unlocked classification only determines whether a lock path
     * can exist. Readiness is decided from the shared locked snapshot below. */
    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        int root_missing;

        err = selected_root_is_missing(&root_missing);
        if (err != CUP_OK) {
            return err;
        }
        if (root_missing) {
            context->runtime_available = 0;
            return CUP_OK;
        }
    }

    err = acquire_runtime_lock(context, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_locked_runtime(&runtime_status);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    if (runtime_status == LAYOUT_RUNTIME_MISSING) {
        command_context_end(context);
        err = prepare_context(context, target_override);
        if (err == CUP_OK) {
            context->runtime_available = 0;
        }
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

    err = state_load(&context->state, &status, &context->state_identity, stderr);
    if (err != CUP_OK) {
        return err;
    }

    if (status == STATE_FILE_MISSING) {
        fprintf(stderr, "Error: state.txt is missing. Run 'cup doctor' and 'cup repair'.\n");
        return CUP_ERR_INCONSISTENT_STATE;
    }

    err = state_validate(&context->state, stderr);
    if (err != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    err = state_validate_current_host(&context->state, context->host_platform, stderr);
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
