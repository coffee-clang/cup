#ifndef CUP_INTERRUPT_H
#define CUP_INTERRUPT_H

/*
 * Process-wide interrupt observation for one mutating command. Native handlers only record
 * intent; transactional code decides where cancellation is safe.
 */

#include "error.h"

/* Install native handlers and preserve the process' previous disposition. */
CupError interrupt_enable(void);

/* Restore the native dispositions saved by interrupt_enable(). */
void interrupt_disable(void);

/* Clear an observed request at a known command boundary. */
void interrupt_clear(void);

/* Return nonzero after SIGINT/SIGTERM or a supported console event. */
int interrupt_requested(void);

#endif /* CUP_INTERRUPT_H */
