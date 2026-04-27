#ifndef CUP_UTIL_H
#define CUP_UTIL_H

#include <stddef.h>

#include "error.h"

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif /* CUP_UTIL_H */