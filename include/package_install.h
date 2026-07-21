#ifndef CUP_PACKAGE_INSTALL_H
#define CUP_PACKAGE_INSTALL_H

/*
 * Module contract: Reusable single-package installation operation used by
 * public install handlers and stable-update plans.
 */

#include "error.h"

CupError package_install(const char *component,
                         const char *selector,
                         const char *target_override,
                         const char *format_override);

CupError package_install_update_scope(const char *component,
                                      const char *tool,
                                      const char *target_override,
                                      const char *expected_active,
                                      int *installed,
                                      int *active_moved);

#endif /* CUP_PACKAGE_INSTALL_H */
