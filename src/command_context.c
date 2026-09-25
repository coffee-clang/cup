/*
 * Owns the shared lifetime of one CLI command: selected-root validation, platform resolution,
 * lock acquisition, state loading and catalog loading.
 */

#include "command_context.h"

#include "generation.h"
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
    state_init(&context->state);
    package_catalog_init(&context->catalog);
    return resolve_platforms(context, target_override);
}

static CupError validate_assets(void) {
    GenerationInspection inspection;
    CupError err = generation_inspect(&inspection);

    if (err != CUP_OK) return err;
    if (generation_has_installed_assets(&inspection)) {
        return generation_installed_is_valid(&inspection) ? CUP_OK : CUP_ERR_VALIDATION;
    }
#if CUP_VERSION_OFFICIAL
    return CUP_ERR_VALIDATION;
#else
    return CUP_OK;
#endif
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

    state_init(&state);
    err = state_save(&state, NULL, NULL);
    state_free(&state);
    if (err == CUP_ERR_COMMIT) {
        fprintf(stderr,
                "Error: initial state.txt may already be published, but its durability could not "
                "be confirmed. Run 'cup doctor' before retrying.\n");
    }
    return err;
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
static CupError inspect_locked_runtime(LayoutRuntimeStatus *status, int require_clear_journal) {
    CupError err = layout_root_snapshot_validate();

    if (err == CUP_OK && require_clear_journal) {
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

/* Only a proven-missing root may be created before locking. Existing roots are always locked
 * before repair or initialization. */
static CupError acquire_bootstrap_lock(CommandContext *context) {
    CupError err = acquire_runtime_lock(context, SYSTEM_LOCK_EXCLUSIVE);
    SystemPathKind root_kind;
    char root_path[MAX_PATH_LEN];

    if (err != CUP_ERR_FILESYSTEM) {
        return err;
    }

    /* Do not treat a filesystem error as absence. Pre-lock creation is allowed only after a
     * no-follow missing-root classification. */
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

static CupError selected_root_is_missing(int *missing);

static CupError command_context_begin_internal(CommandContext *context,
                                               const char *target_override,
                                               SystemLockMode mode,
                                               int allow_initialize) {
    LayoutRuntimeStatus runtime_status = LAYOUT_RUNTIME_MISSING;
    SystemLockMode lock_mode = mode;
    CupError err;

    if (context == NULL || (mode != SYSTEM_LOCK_SHARED && mode != SYSTEM_LOCK_EXCLUSIVE)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = prepare_context(context, target_override);
    if (err == CUP_OK) {
        int root_missing = 0;

        err = selected_root_is_missing(&root_missing);
        if (err == CUP_OK && root_missing) {
            if (!allow_initialize) {
                return CUP_ERR_NOT_INSTALLED;
            }
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
        } else if (err == CUP_OK) {
            err = acquire_runtime_lock(context, lock_mode);
        }
    }
    if (err != CUP_OK) {
        return err;
    }

    err = inspect_locked_runtime(&runtime_status, 1);
    if (err != CUP_OK) {
        command_context_end(context);
        return err;
    }

    /* A shared preflight never initializes under a shared lock. Reacquire the exclusive lock and
     * rebuild the entire locked snapshot when the runtime disappeared concurrently. */
    if (runtime_status == LAYOUT_RUNTIME_MISSING && lock_mode == SYSTEM_LOCK_SHARED) {
        command_context_end(context);
        if (!allow_initialize) {
            return CUP_ERR_NOT_INSTALLED;
        }
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
        err = inspect_locked_runtime(&runtime_status, 1);
        if (err != CUP_OK) {
            command_context_end(context);
            return err;
        }
    }

    if (lock_mode == SYSTEM_LOCK_EXCLUSIVE) {
        /* Runtime creation is a caller intent, not a consequence of asking for a mutation lock. */
        if (runtime_status == LAYOUT_RUNTIME_MISSING && !allow_initialize) {
            command_context_end(context);
            return CUP_ERR_NOT_INSTALLED;
        }
        if (runtime_status == LAYOUT_RUNTIME_MISSING) {
            err = validate_assets();
        }
        if (err == CUP_OK && allow_initialize) {
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

CupError command_context_begin(CommandContext *context,
                               const char *target_override,
                               SystemLockMode mode) {
    return command_context_begin_internal(context, target_override, mode, 0);
}

CupError command_context_begin_initialize(CommandContext *context,
                                          const char *target_override,
                                          SystemLockMode mode) {
    return command_context_begin_internal(context, target_override, mode, 1);
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
        int root_missing = 0;

        err = selected_root_is_missing(&root_missing);
        if (err == CUP_OK && root_missing) {
            context->runtime_available = 0;
            return CUP_OK;
        }
    }
    if (err != CUP_OK) {
        return err;
    }

    err = acquire_runtime_lock(context, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        return err;
    }
    err = inspect_locked_runtime(&runtime_status, 0);
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

    state_free(&context->state);
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
