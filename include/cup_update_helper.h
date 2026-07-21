#ifndef CUP_UPDATE_HELPER_H
#define CUP_UPDATE_HELPER_H

/*
 * Coordinates the native helper used when CUP must replace its own executable after the
 * parent process exits. The parent prepares and starts a managed copy; only that copy may
 * enter cup_update_helper_run().
 */

#include "error.h"

/* Parent-side preparation and handoff. */
CupError cup_update_helper_prepare(void);
CupError cup_update_helper_start(const char *token);

/* Detached helper entry point. */
CupError cup_update_helper_run(const char *token, const char *wait_value);

#endif /* CUP_UPDATE_HELPER_H */
