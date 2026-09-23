#ifndef CUP_STATE_H
#define CUP_STATE_H

/*
 * Dynamic installed/default state with atomic state.txt persistence. The authenticated root owns
 * the host platform; state.txt stores component/target/tool/version only.
 */

#include <stddef.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "package.h"
#include "system.h"

typedef struct {
    PackageIdentity *installed;
    size_t installed_count;
    size_t installed_capacity;

    PackageIdentity defaults[MAX_STATE_DEFAULTS];
    size_t default_count;
} CupState;

typedef enum {
    STATE_FILE_MISSING,
    STATE_FILE_LOADED
} StateFileStatus;

void state_init(CupState *state);
void state_free(CupState *state);

CupError state_load(CupState *state,
                    StateFileStatus *status,
                    SystemPathIdentity *source_identity,
                    FILE *diagnostics);
CupError state_validate(const CupState *state, FILE *diagnostics);
CupError state_measure_persistent(const CupState *state, size_t *size);
size_t state_count_foreign_hosts(const CupState *state, const char *current_host);
CupError state_validate_current_host(const CupState *state,
                                     const char *current_host,
                                     FILE *diagnostics);
CupError state_save(const CupState *state,
                    const SystemPathIdentity *expected_identity,
                    SystemPathIdentity *published_identity);

int state_find_installed(const CupState *state, const PackageIdentity *identity);
CupError state_add_installed(CupState *state, const PackageIdentity *identity);
CupError state_remove_installed(CupState *state, const PackageIdentity *identity);

const PackageIdentity *state_get_default(const CupState *state, const PackageScope *scope);
CupError state_get_tool_reference(const CupState *state,
                                  const PackageScope *scope,
                                  const char *tool,
                                  PackageIdentity *reference,
                                  int *reference_is_default);
CupError state_set_default(CupState *state, const PackageIdentity *identity);
CupError state_clear_default(CupState *state, const PackageScope *scope);
CupError state_clear_matching_default(CupState *state, const PackageIdentity *identity);

#endif /* CUP_STATE_H */
