#ifndef CUP_BOOTSTRAP_H
#define CUP_BOOTSTRAP_H

/* Internal native installer entry point used only by verified transport scripts. */

#include "error.h"

CupError bootstrap_start(const char *source_directory,
                         const char *running_binary,
                         const char *base);

#endif /* CUP_BOOTSTRAP_H */
