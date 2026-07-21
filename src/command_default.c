/*
 * Selects one validated installed package as the active default of its scope.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "layout.h"
#include "package.h"
#include "package_catalog.h"
#include "package_metadata.h"
#include "package_request.h"
#include "package_selector.h"
#include "path.h"
#include "registry.h"
#include "runtime_journal.h"
#include "state.h"
#include "wrappers.h"

#include <stdio.h>
#include <string.h>

/* Load only the catalog data required to resolve a symbolic release. */
static CupError load_default_context(CommandContext *context,
                                     const PackageRequest *request,
                                     const char *target_override) {
    CupError err;

    err = command_context_begin(context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) {
        err = runtime_journal_require_none();
    }
    if (err == CUP_OK) {
        err = command_context_load_state(context);
    }
    if (err == CUP_OK && package_release_is_stable(request->selector.release)) {
        err = command_context_load_catalog(context);
    }
    return err;
}

/* Turn the public request into the concrete installed identity for this scope. */
static CupError resolve_default_identity(CommandContext *context,
                                         const char *component,
                                         PackageRequest *request,
                                         PackageIdentity *package) {
    CupError err;

    err = package_request_resolve(context->has_catalog ? &context->catalog : NULL,
                                  component,
                                  context->host_platform,
                                  context->target_platform,
                                  request);
    if (err != CUP_OK) {
        return err;
    }

    err = package_identity_init(package,
                                component,
                                request->selector.tool,
                                context->host_platform,
                                context->target_platform,
                                request->resolved_release);
    if (err != CUP_OK) {
        return err;
    }
    return installed_package_require_valid(&context->state, package);
}

/* Validate the derived wrappers before committing the new authoritative state. */
static CupError commit_default(CommandContext *context,
                               const PackageIdentity *package,
                               WrapperPlan *wrappers) {
    CupState candidate = context->state;
    CupError err;

    err = state_set_active(&candidate, package);
    if (err == CUP_OK) {
        err = wrapper_plan_build(wrappers, &candidate);
    }
    if (err != CUP_OK) {
        return err;
    }

    /* state.txt is the commit point; a later wrapper failure is a partial commit. */
    context->state = candidate;
    err = state_save(&context->state);
    if (err != CUP_OK) {
        return err;
    }

    err = wrapper_plan_apply(wrappers);
    if (err != CUP_OK) {
        fprintf(stderr,
                "Error: the default was saved, but its wrappers could not "
                "be rebuilt. Run 'cup repair'.\n");
        return CUP_ERR_COMMIT;
    }
    return CUP_OK;
}

CupError command_default(const char *component, const char *selector, const char *target_override) {
    CommandContext context = {0};
    PackageRequest request;
    PackageIdentity package;
    WrapperPlan wrappers;
    CupError err;

    wrapper_plan_init(&wrappers);
    if (component == NULL || selector == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_request_parse(component, selector, &request);
    if (err == CUP_OK) {
        err = load_default_context(&context, &request, target_override);
    }
    if (err == CUP_OK) {
        err = resolve_default_identity(&context, component, &request, &package);
    }
    if (err == CUP_OK) {
        err = commit_default(&context, &package, &wrappers);
    }
    if (err == CUP_OK) {
        printf("Default %s for host '%s', target '%s' set to ",
               component,
               context.host_platform,
               context.target_platform);
        package_request_print(stdout, &request);
        printf(".\n");
    }

    wrapper_plan_free(&wrappers);
    command_context_end(&context);
    return err;
}
