#ifndef CUP_WRAPPERS_H
#define CUP_WRAPPERS_H

/*
 * Module contract: Immutable plans for managed launchers derived from valid
 * active packages. Planning is separated from filesystem application so
 * doctor can diagnose the same desired state that repair applies.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "state.h"

/* One managed launcher name and its validated package-relative target. */
typedef struct {
    char name[MAX_COMMAND_NAME_LEN];
    char target[MAX_PATH_LEN];
} WrapperSpec;

/* Dynamically sized desired launcher set owned by the caller. */
typedef struct {
    WrapperSpec *items;
    size_t count;
    size_t capacity;
} WrapperPlan;

/* Initialize or release all storage owned by a WrapperPlan. */
void wrapper_plan_init(WrapperPlan *plan);
void wrapper_plan_free(WrapperPlan *plan);

/* Rebuild a plan with the exact launcher set represented by all active packages. */
CupError wrapper_plan_build(WrapperPlan *plan, const CupState *state);

/* Rebuild a plan with launchers from one already selected active package. */
CupError wrapper_plan_build_active(WrapperPlan *plan, const PackageIdentity *active_identity);

/* Apply one validated plan and remove managed launchers absent from it. */
CupError wrapper_plan_apply(const WrapperPlan *plan);

/* Compare the managed bin directory with the exact desired launcher set. */
CupError wrapper_plan_expected_matches(const WrapperPlan *plan, int *matches);

/* Count missing, invalid, conflicting, or unexpected managed launchers. */
CupError wrapper_plan_check(const WrapperPlan *plan, size_t *issue_count);

#endif /* CUP_WRAPPERS_H */
