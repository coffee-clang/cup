#ifndef CUP_UPDATE_HELPER_H
#define CUP_UPDATE_HELPER_H

#include "error.h"

/* Prepare and start the native helper copy that completes CUP update post-exit. */
CupError cup_update_helper_prepare(void);
CupError cup_update_helper_start(const char *token);

/* Private entry point executed only by the managed helper copy. */
CupError cup_update_helper_run(const char *token, const char *wait_value);

#endif /* CUP_UPDATE_HELPER_H */
