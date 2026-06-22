#ifndef CUP_ENTRYPOINTS_H
#define CUP_ENTRYPOINTS_H

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "state.h"

typedef struct {
    char name[MAX_ENTRYPOINT_NAME_LEN];
    char target[MAX_PATH_LEN];
} EntryPointSpec;

typedef struct {
    EntryPointSpec *items;
    size_t count;
    size_t capacity;
} EntryPointPlan;

void entrypoint_plan_init(EntryPointPlan *plan);
void entrypoint_plan_free(EntryPointPlan *plan);

/* Rebuild an initialized plan with the exact wrapper set represented by state. */
CupError entrypoint_plan_build(EntryPointPlan *plan, const CupState *state);

/* Rebuild an initialized plan with the wrappers from one default package. */
CupError entrypoint_plan_build_default(EntryPointPlan *plan,
    const StateEntry *default_entry);

/* Apply or diagnose one already validated plan. */
CupError entrypoint_plan_apply(const EntryPointPlan *plan);
CupError entrypoint_plan_expected_matches(const EntryPointPlan *plan,
    int *matches);
CupError entrypoint_plan_check(const EntryPointPlan *plan,
    size_t *issue_count);

#endif /* CUP_ENTRYPOINTS_H */
