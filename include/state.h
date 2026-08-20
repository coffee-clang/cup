#ifndef CUP_STATE_H
#define CUP_STATE_H

/*
 * Bounded installed/default state with atomic state.txt persistence. Memory stores concrete
 * package identities; state.txt stores their canonical tool@version selectors.
 */

#include <stddef.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "package.h"
#include "system.h"

/* In-memory representation of the complete persistent state. */
typedef struct {
    PackageIdentity installed[MAX_INSTALLED];
    size_t installed_count;

    PackageIdentity defaults[MAX_STATE_DEFAULTS];
    size_t default_count;
} CupState;

typedef enum {
    STATE_FILE_MISSING,
    STATE_FILE_LOADED
} StateFileStatus;

/*
 * Load the complete file after syntax validation. Missing state is reported separately, and
 * malformed input is never accepted partially. Call state_validate() when semantic consistency
 * is required. When a physical state snapshot is read successfully, source_identity receives its
 * exact regular-file identity even if later parsing fails; otherwise the optional output is
 * cleared. Provided model/status outputs are initialized to empty/MISSING before path access.
 * Repair uses the observed identity to preserve the object it actually diagnosed. Pass NULL
 * diagnostics when the caller aggregates its own messages.
 */
CupError state_load(CupState *state,
                    StateFileStatus *status,
                    SystemPathIdentity *source_identity,
                    FILE *diagnostics);

/* Validate capacities, uniqueness, identities and default references; diagnostics may be NULL. */
CupError state_validate(const CupState *state, FILE *diagnostics);

/* Count records belonging to hosts other than the current host. */
size_t state_count_foreign_hosts(const CupState *state, const char *current_host);

/* Reject structurally valid state that contains foreign-host records. */
CupError state_validate_current_host(const CupState *state,
                                     const char *current_host,
                                     FILE *diagnostics);

/*
 * Publish the complete state. NULL expected_identity means create-only initialization; otherwise
 * the exact loaded regular file must still be present. published_identity is cleared on failure
 * and receives the new state-file identity on success; it may alias expected_identity.
 */
CupError state_save(const CupState *state,
                    const SystemPathIdentity *expected_identity,
                    SystemPathIdentity *published_identity);

/* Installed-identity lookup and bounded mutation. */
int state_find_installed(const CupState *state, const PackageIdentity *identity);
CupError state_add_installed(CupState *state, const PackageIdentity *identity);
CupError state_remove_installed(CupState *state, const PackageIdentity *identity);

/* Default lookup and one-identity-per-scope mutation. */
const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope);
CupError state_set_default(CupState *state, const PackageIdentity *identity);
CupError state_clear_default(CupState *state, const PackageScope *scope);

/* Clear the default package only when it still refers to the expected identity. */
CupError state_clear_matching_default(CupState *state, const PackageIdentity *identity);

#endif /* CUP_STATE_H */
