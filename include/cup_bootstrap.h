#ifndef CUP_BOOTSTRAP_H
#define CUP_BOOTSTRAP_H

/* Internal initial-install entry point used only by verified transport scripts. */

#include "error.h"

CupError cup_bootstrap_start(const char *source_directory, const char *running_binary);

#endif /* CUP_BOOTSTRAP_H */
