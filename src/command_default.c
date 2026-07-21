/*
 * Selects one validated installed package as the active default of its scope.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "runtime_journal.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "package_metadata.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

CupError command_default(const char *component, const char *selector, const char *target_override) {
    CommandContext context = {0};
    PackageRequest request;
    PackageIdentity package;
    CupState candidate;
    WrapperPlan wrappers;
    CupError err;

    wrapper_plan_init(&wrappers);

    if (component == NULL || selector == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_request_parse(component, selector, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin(&context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }

    err = runtime_journal_require_none();
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    if (package_release_is_stable(request.selector.release)) {
        err = command_context_load_catalog(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = package_request_resolve(context.has_catalog ? &context.catalog : NULL,
                                  component,
                                  context.host_platform,
                                  context.target_platform,
                                  &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package,
                                component,
                                request.selector.tool,
                                context.host_platform,
                                context.target_platform,
                                request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    err = installed_package_require_valid(&context.state, &package);
    if (err != CUP_OK) {
        goto done;
    }

    candidate = context.state;
    err = state_set_active(&candidate, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = wrapper_plan_build(&wrappers, &candidate);
    if (err != CUP_OK) {
        goto done;
    }

    context.state = candidate;
    err = state_save(&context.state);
    if (err != CUP_OK) {
        goto done;
    }

    err = wrapper_plan_apply(&wrappers);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: the default was saved, but its wrappers could not "
                "be rebuilt. Run 'cup repair'.\n");
        err = CUP_ERR_COMMIT;
        goto done;
    }

    printf("Default %s for host '%s', target '%s' set to ",
           component,
           context.host_platform,
           context.target_platform);
    package_request_print(stdout, &request);
    printf(".\n");

done:
    wrapper_plan_free(&wrappers);
    command_context_end(&context);
    return err;
}

/* Current tool information. */
