#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#include "error.h"

CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

#endif