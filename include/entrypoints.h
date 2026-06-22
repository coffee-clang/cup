#ifndef CUP_ENTRYPOINTS_H
#define CUP_ENTRYPOINTS_H

#include <stddef.h>

#include "error.h"
#include "state.h"

/* Validate that all managed wrapper names and targets are coherent. */
CupError entrypoints_validate(const CupState *state);

/* Rebuild all managed wrappers from the current default entries. */
CupError entrypoints_sync(const CupState *state);

/* Diagnose missing, altered or stale wrappers without modifying them. */
CupError entrypoints_check(const CupState *state, size_t *issue_count);

#endif /* CUP_ENTRYPOINTS_H */
