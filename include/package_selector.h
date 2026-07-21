#ifndef CUP_PACKAGE_SELECTOR_H
#define CUP_PACKAGE_SELECTOR_H

/*
 * Module contract: Validation and construction of symbolic or concrete
 * '<tool>@<release>' strings used by CLI and state layers.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Symbolic or concrete package selection accepted at command boundaries. */
typedef struct {
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
} PackageSelector;

/* Validate and initialize one package selector. */
CupError package_selector_init(PackageSelector *selector, const char *tool, const char *release);

/* Parse and validate a canonical '<tool>@<release>' selector. */
CupError package_selector_parse(PackageSelector *selector, const char *text);

/* Format a previously validated selector. */
CupError package_selector_format(const PackageSelector *selector, char *buffer, size_t size);

/* Return whether the selector uses a symbolic release. */
int package_selector_is_symbolic(const PackageSelector *selector);

/* Return whether a release uses the exact symbolic 'stable' name. */
int package_release_is_stable(const char *release);

/* Parse and validate both fields of a '<tool>@<release>' string. */
CupError package_selector_parse_parts(
    const char *text, char *tool, size_t tool_size, char *release, size_t release_size);

/* Build a canonical selector from already validated tool and release values. */
CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release);

#endif /* CUP_PACKAGE_SELECTOR_H */
