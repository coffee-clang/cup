#ifndef CUP_INTERRUPT_H
#define CUP_INTERRUPT_H

/*
 * Process-wide interrupt observation for one state-changing command. Native handlers only record
 * intent; transactional code decides where cancellation is safe.
 */

#include "error.h"

/* Install native handlers and preserve the process' previous disposition. */
CupError interrupt_enable(void);

/* Restore the native dispositions saved by interrupt_enable(). */
void interrupt_disable(void);

/* Return nonzero after SIGINT/SIGTERM or a supported console event. */
int interrupt_requested(void);

/* Convert a pending request into the canonical command error at a declared safe point. */
CupError interrupt_safe_point(void);

#endif /* CUP_INTERRUPT_H */
