#ifndef CUP_PLATFORM_H
#define CUP_PLATFORM_H

#include <stddef.h>

#include "error.h"

/* Platform identifiers use the '<os>-<arch>' form, for example 'linux-x64'. */
CupError platform_get_host(char *buffer, size_t size);
CupError platform_validate(const char *platform);

#endif /* CUP_PLATFORM_H */
