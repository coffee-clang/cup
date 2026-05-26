#ifndef CUP_ENTRY_H
#define CUP_ENTRY_H

#include <stddef.h>

#include "error.h"

int is_stable_release(const char *release);
CupError parse_entry(const char *entry, char *tool, size_t tool_size, char *release, size_t release_size);
CupError build_entry(char *buffer, size_t size, const char *tool, const char *release);

#endif /* CUP_ENTRY_H */