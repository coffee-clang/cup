#ifndef CUP_ENTRY_H
#define CUP_ENTRY_H

#include <stddef.h>

#include "error.h"

/* Check whether a release uses the symbolic 'stable' name. */
int entry_is_stable(const char *release);

/* Parse a '<tool>@<release>' string. */
CupError entry_parse(const char *entry, char *tool, size_t tool_size,
    char *release, size_t release_size);

/* Build a '<tool>@<release>' string. */
CupError entry_build(char *buffer, size_t size, const char *tool, const char *release);

#endif /* CUP_ENTRY_H */
