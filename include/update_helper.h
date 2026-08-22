#ifndef CUP_UPDATE_HELPER_H
#define CUP_UPDATE_HELPER_H

/*
 * Coordinates the native helper that replaces cup after the parent process exits. The parent
 * prepares and starts a managed copy; only that copy may enter update_helper_run().
 */

#include "error.h"
#include "system.h"

/* Parent-side preparation and handoff. */
CupError update_helper_prepare(void);
CupError update_helper_prepare_from(const char *source_binary);
CupError update_helper_start(const char *root, const char *token, SystemLock *lock);

/* Detached helper entry point. */
CupError update_helper_run(const char *root,
                           const char *token,
                           const char *parent_signal_value,
                           const char *authority_value);

#endif /* CUP_UPDATE_HELPER_H */
