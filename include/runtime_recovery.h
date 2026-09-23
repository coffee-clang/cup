#ifndef CUP_RUNTIME_RECOVERY_H
#define CUP_RUNTIME_RECOVERY_H

/* Recovers the one shared transaction.txt owner under an already-held exclusive runtime lock. */

#include "error.h"
#include "runtime_journal.h"
#include "system.h"

CupError runtime_recover_pending(const SystemLock *lock, RuntimeJournalKind *recovered_kind);

#endif /* CUP_RUNTIME_RECOVERY_H */
