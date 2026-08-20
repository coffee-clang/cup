#ifndef CUP_PACKAGE_SELECTOR_H
#define CUP_PACKAGE_SELECTOR_H

/*
 * Validation and construction of symbolic or concrete '<tool>@<release>' selectors used by CLI
 * and state layers.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Symbolic or concrete package selection accepted at command boundaries. */
typedef struct {
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
} PackageSelector;

/* Parse and validate a canonical '<tool>@<release>' selector. */
CupError package_selector_parse(PackageSelector *selector, const char *text);

/* Return whether a release uses the exact symbolic 'stable' name. */
int package_release_is_stable(const char *release);

/* Validate one canonical concrete release that may be persisted. */
CupError package_release_validate_concrete(const char *release);

/* Split one exact non-empty '<tool>@<release>' string without trimming or normalization. */
CupError package_selector_parse_parts(
    const char *text, char *tool, size_t tool_size, char *release, size_t release_size);

/* Build a canonical selector from already validated tool and release values. */
CupError package_selector_format_parts(char *buffer,
                                       size_t size,
                                       const char *tool,
                                       const char *release);

#endif /* CUP_PACKAGE_SELECTOR_H */
