#ifndef CUP_PLATFORM_H
#define CUP_PLATFORM_H

#include <stddef.h>

#include "error.h"

CupError get_host_platform(char *buffer, size_t size);
CupError validate_platform(const char *platform);

#endif