#ifndef CUP_STATE_H
#define CUP_STATE_H

/*
 * Module contract: Bounded installed/active state model and atomic
 * state.txt persistence. The in-memory model uses concrete package
 * identities; the boundary file retains canonical tool@version values.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "package.h"

/* In-memory representation of the complete persistent state. */
typedef struct {
    PackageIdentity installed[MAX_INSTALLED];
    size_t installed_count;

    PackageIdentity active[MAX_ACTIVE_PACKAGES];
    size_t active_count;
} CupState;

typedef enum {
    STATE_FILE_MISSING,
    STATE_FILE_LOADED
} StateFileStatus;

/*
 * Load the complete syntactically valid file. Missing state is reported
 * separately and malformed input is never accepted partially. Call
 * state_validate() when semantic consistency is required; repair uses the
 * parsed form to reconcile stale active selections and installed identities.
 */
CupError state_load(CupState *state, StateFileStatus *status);

/* Validate capacities, uniqueness, identities, and active references. */
CupError state_validate(const CupState *state);

/* Count records belonging to hosts other than the current host. */
size_t state_count_foreign_hosts(const CupState *state, const char *current_host);

/* Reject structurally valid state that contains foreign-host records. */
CupError state_validate_current_host(const CupState *state, const char *current_host);

/* Atomically replace state.txt and report uncertain durability as commit error. */
CupError state_save(const CupState *state);

/* Installed-identity lookup and bounded mutation. */
int state_find_installed(const CupState *state, const PackageIdentity *identity);
CupError state_add_installed(CupState *state, const PackageIdentity *identity);
CupError state_remove_installed(CupState *state, const PackageIdentity *identity);

/* Active lookup and one-identity-per-scope mutation. */
int state_find_active(const CupState *state, const PackageScope *scope);
const PackageIdentity *state_get_active(const CupState *state, const PackageScope *scope);
CupError state_set_active(CupState *state, const PackageIdentity *identity);
CupError state_clear_active(CupState *state, const PackageScope *scope);

/* Clear the active package only when it still refers to the expected identity. */
CupError state_clear_matching_active(CupState *state, const PackageIdentity *identity);

#endif /* CUP_STATE_H */
