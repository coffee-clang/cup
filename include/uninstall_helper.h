#ifndef CUP_UNINSTALL_HELPER_H
#define CUP_UNINSTALL_HELPER_H

/* Native detached helper that takes exclusive handoff authority and removes one managed root. */

#include "error.h"
#include "system.h"

CupError uninstall_helper_start(const char *root,
                                const char *detached_root,
                                const char *token,
                                SystemLock *lock);
/* Remove only the reserved helper derived from this root/token pair. This is valid only while the
 * caller still owns the active canonical exclusive lock, before any accepted detach handoff. */
CupError uninstall_helper_remove_stale(const char *root,
                                       const char *token,
                                       const SystemLock *lock);
CupError uninstall_helper_run(const char *root,
                              const char *detached_root,
                              const char *token,
                              const char *parent_signal_value,
                              const char *authority_value);

#endif /* CUP_UNINSTALL_HELPER_H */
