/*
 * Resolves the public install grammar into one fully validated package plan. Profiles
 * intentionally use user preferences; toolchains are explicit plans and never consult those
 * preferences.
 */

#include "commands.h"

#include "command_context.h"
#include "package_selector.h"
#include "package_request.h"
#include "package_install.h"
#include "installed_package.h"
#include "install_policy.h"
#include "tool_preferences.h"
#include "package_catalog.h"
#include "registry.h"
#include "state.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char selector[MAX_SELECTOR_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    PackageArtifactSpec artifact_spec;
    int available;
} InstallPlanItem;

typedef enum {
    INSTALL_PLAN_SINGLE,
    INSTALL_PLAN_PROFILE,
    INSTALL_PLAN_TOOLCHAIN
} InstallPlanKind;

typedef struct {
    InstallPlanItem items[MAX_INSTALL_LIST_ITEMS];
    size_t count;
    InstallPlanKind kind;
    char description[MAX_IDENTIFIER_LEN];
} InstallPlan;

static CupError install_plan_add(InstallPlan *plan, const char *component, const char *selector) {
    InstallPlanItem *item;

    if (plan == NULL || text_is_empty(component) || text_is_empty(selector) ||
        plan->count >= MAX_INSTALL_LIST_ITEMS) {
        return CUP_ERR_INVALID_INPUT;
    }
    item = &plan->items[plan->count++];
    memset(item, 0, sizeof(*item));
    if (text_copy(item->component, sizeof(item->component), component) != CUP_OK ||
        text_copy(item->selector, sizeof(item->selector), selector) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }
    return CUP_OK;
}

static CupError install_plan_add_component(InstallPlan *plan,
                                           const InstallPolicy *config,
                                           const ToolPreferences *preferences,
                                           const char *host_platform,
                                           const char *target_platform,
                                           const char *component,
                                           const char *explicit_entry) {
    char selector[MAX_SELECTOR_LEN];
    CupError err;

    if (!text_is_empty(explicit_entry)) {
        err = text_copy(selector, sizeof(selector), explicit_entry);
    } else {
        char tool[MAX_IDENTIFIER_LEN];
        ToolPreferenceSource source;

        err = tool_preferences_resolve(config,
                                       preferences,
                                       host_platform,
                                       target_platform,
                                       component,
                                       tool,
                                       sizeof(tool),
                                       &source);
        if (err == CUP_OK) {
            err = package_selector_format_parts(selector, sizeof(selector), tool, "stable");
        }
    }
    return err == CUP_OK ? install_plan_add(plan, component, selector) : err;
}

/* Plan construction. Profiles and toolchains expand into a bounded list before any package is
 * downloaded. */
static CupError install_plan_build(InstallPlan *plan,
                                   const InstallPolicy *config,
                                   const ToolPreferences *preferences,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *selector_input,
                                   const char *value_input) {
    char selection[MAX_SELECTOR_LEN];
    char value[MAX_SELECTOR_LEN] = "";
    CupError err;
    size_t i;

    /* The public parser supplies canonical command grammar before runtime state is touched. */
    memset(plan, 0, sizeof(*plan));
    if (text_copy(selection, sizeof(selection), selector_input) != CUP_OK ||
        (!text_is_empty(value_input) && text_copy(value, sizeof(value), value_input) != CUP_OK)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    /* A component creates one selection, optionally overridden by an explicit tool selector. */
    if (registry_is_component(selection)) {
        plan->kind = INSTALL_PLAN_SINGLE;
        if (text_copy(plan->description, sizeof(plan->description), selection) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        return install_plan_add_component(plan,
                                          config,
                                          preferences,
                                          host_platform,
                                          target_platform,
                                          selection,
                                          text_is_empty(value) ? NULL : value);
    }

    /* Profiles expand components and therefore apply scoped user preferences. */
    if (strcmp(selection, "profile") == 0) {
        const InstallNamedList *profile;

        if (text_is_empty(value)) {
            fprintf(stderr, "Error: install profile requires a profile name.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        profile = install_policy_find_profile(config, value);
        if (profile == NULL) {
            fprintf(stderr, "Error: unknown install profile '%s'.\n", value);
            return CUP_ERR_INVALID_INPUT;
        }
        plan->kind = INSTALL_PLAN_PROFILE;
        if (text_copy(plan->description, sizeof(plan->description), value) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        for (i = 0; i < profile->item_count; ++i) {
            err = install_plan_add_component(
                plan, config, preferences, host_platform, target_platform, profile->items[i], NULL);
            if (err != CUP_OK) {
                return err;
            }
        }
        return CUP_OK;
    }

    /* Toolchains name concrete tools and intentionally bypass user preferences. */
    if (strcmp(selection, "toolchain") == 0) {
        const InstallNamedList *toolchain;

        if (text_is_empty(value)) {
            fprintf(stderr, "Error: install toolchain requires a toolchain name.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        toolchain = install_policy_find_toolchain(config, value);
        if (toolchain == NULL) {
            fprintf(stderr, "Error: unknown toolchain '%s'.\n", value);
            return CUP_ERR_INVALID_INPUT;
        }
        plan->kind = INSTALL_PLAN_TOOLCHAIN;
        if (text_copy(plan->description, sizeof(plan->description), value) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        for (i = 0; i < toolchain->item_count; ++i) {
            char component[MAX_IDENTIFIER_LEN];
            char tool_selector[MAX_SELECTOR_LEN];

            err = registry_find_tool_component(toolchain->items[i], component, sizeof(component));
            if (err == CUP_OK) {
                err = package_selector_format_parts(
                    tool_selector, sizeof(tool_selector), toolchain->items[i], "stable");
            }
            if (err == CUP_OK) {
                err = install_plan_add(plan, component, tool_selector);
            }
            if (err != CUP_OK) {
                return err;
            }
        }
        return CUP_OK;
    }

    /*
     * Tool-first forms resolve their unique component through the compiled registry. A second
     * positional value is never accepted because it would make the grammar ambiguous.
     */
    if (text_is_empty(value)) {
        char tool[MAX_IDENTIFIER_LEN];
        char release[MAX_IDENTIFIER_LEN];
        char component[MAX_IDENTIFIER_LEN];

        err = package_selector_parse_parts(selection, tool, sizeof(tool), release, sizeof(release));
        if (err == CUP_OK) {
            err = registry_find_tool_component(tool, component, sizeof(component));
        }
        if (err == CUP_OK) {
            plan->kind = INSTALL_PLAN_SINGLE;
            err = text_copy(plan->description, sizeof(plan->description), selection);
        }
        return err == CUP_OK ? install_plan_add(plan, component, selection) : err;
    }

    fprintf(stderr, "Error: unsupported component, tool or install group '%s'.\n", selection);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

/* Full preflight. Every catalog selection and installed-package condition is validated before the
 * first side effect. */
static CupError install_plan_resolve_format(InstallPlanItem *item,
                                            const PackageRequest *request,
                                            const CommandContext *context,
                                            const char *format_override,
                                            char *format,
                                            size_t format_size,
                                            int *available) {
    CupError err;

    *available = 1;
    if (!text_is_empty(format_override)) {
        err = package_catalog_has_format(&context->catalog,
                                         item->component,
                                         request->selector.tool,
                                         context->host_platform,
                                         context->target_platform,
                                         format_override,
                                         available);
        if (err != CUP_OK || !*available) {
            return err;
        }
        return text_copy(format, format_size, format_override);
    }

    return package_catalog_get_default_format(&context->catalog,
                                              format,
                                              format_size,
                                              item->component,
                                              request->selector.tool,
                                              context->host_platform,
                                              context->target_platform);
}

static CupError install_plan_check_installed(const InstallPlanItem *item,
                                             const CommandContext *context) {
    const PackageIdentity *identity = &item->artifact_spec.identity;
    char selector[MAX_SELECTOR_LEN];
    CupError err;

    if (state_find_installed(&context->state, identity) < 0) {
        return CUP_OK;
    }

    err = installed_package_require_valid(&context->state, identity);
    if (err != CUP_OK) {
        if (package_identity_format_selector(identity, selector, sizeof(selector)) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        fprintf(stderr,
                "Error: selected package '%s:%s' is recorded but invalid; "
                "run 'cup doctor' and 'cup repair'.\n",
                identity->component,
                selector);
    }
    return err;
}

static CupError install_plan_validate_item(InstallPlanItem *item,
                                           const CommandContext *context,
                                           const char *format_override,
                                           int *unavailable) {
    PackageRequest request;
    CupError err;
    char format[MAX_IDENTIFIER_LEN];
    int package_available;
    int version_available;
    int format_available;

    *unavailable = 0;
    err = package_request_parse(item->component, item->selector, &request);
    if (err != CUP_OK) {
        return err;
    }
    if (text_copy(item->tool, sizeof(item->tool), request.selector.tool) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    err = package_catalog_has_package(&context->catalog,
                                      item->component,
                                      request.selector.tool,
                                      context->host_platform,
                                      context->target_platform,
                                      &package_available);
    if (err != CUP_OK) {
        return err;
    }
    if (!package_available) {
        item->available = 0;
        *unavailable = 1;
        return CUP_OK;
    }

    err = package_request_resolve(&context->catalog,
                                  item->component,
                                  context->host_platform,
                                  context->target_platform,
                                  &request);
    if (err != CUP_OK) {
        return err;
    }
    err = package_catalog_has_version(&context->catalog,
                                      item->component,
                                      request.selector.tool,
                                      context->host_platform,
                                      context->target_platform,
                                      request.resolved_release,
                                      &version_available);
    if (err != CUP_OK) {
        return err;
    }
    if (!version_available) {
        item->available = 0;
        *unavailable = 1;
        return CUP_OK;
    }

    err = install_plan_resolve_format(item,
                                      &request,
                                      context,
                                      format_override,
                                      format,
                                      sizeof(format),
                                      &format_available);
    if (err != CUP_OK) {
        return err;
    }
    if (!format_available) {
        item->available = 0;
        *unavailable = 1;
        return CUP_OK;
    }

    item->available = 1;
    {
        PackageIdentity identity;

        err = package_identity_init(&identity,
                                    item->component,
                                    request.selector.tool,
                                    context->host_platform,
                                    context->target_platform,
                                    request.resolved_release);
        if (err == CUP_OK) {
            err = package_artifact_spec_build(
                &item->artifact_spec, &context->catalog, &identity, format);
        }
        if (err != CUP_OK) {
            return err;
        }
    }
    return install_plan_check_installed(item, context);
}

static void install_plan_print_unavailable(const InstallPlan *plan, const CommandContext *context) {
    size_t i;

    fprintf(stderr,
            "%s '%s' cannot be installed for host '%s', target '%s':\n",
            plan->kind != INSTALL_PLAN_SINGLE ? "Install group" : "Selection",
            plan->description,
            context->host_platform,
            context->target_platform);
    for (i = 0; i < plan->count; ++i) {
        const InstallPlanItem *item = &plan->items[i];

        fprintf(stderr,
                "  %-16s %-18s %s\n",
                item->component,
                item->tool[0] == '\0' ? item->selector : item->tool,
                item->available ? "available" : "not currently available");
    }
    fprintf(stderr, "No packages were installed.\n");
}

static CupError install_plan_validate(InstallPlan *plan,
                                      CommandContext *context,
                                      const char *format_override) {
    CupError err;
    size_t i;
    size_t unavailable_count = 0;

    for (i = 0; i < plan->count; ++i) {
        int unavailable;

        err = install_plan_validate_item(&plan->items[i], context, format_override, &unavailable);
        if (err != CUP_OK) {
            return err;
        }
        unavailable_count += (size_t)unavailable;
    }

    if (unavailable_count != 0) {
        install_plan_print_unavailable(plan, context);
        return CUP_ERR_NOT_AVAILABLE;
    }
    return CUP_OK;
}

/* Public request planning delegates catalog-pinned artifacts to package installation. */
CupError command_install(const char *selector,
                         const char *value,
                         const char *target_override,
                         const char *format_override) {
    CommandContext context = {0};
    InstallPolicy config;
    ToolPreferences preferences;
    InstallPlan plan;
    CupError err;
    int need_config;
    int need_preferences;
    size_t i;
    size_t installed_count = 0;
    size_t skipped_count = 0;

    /* Public grammar is canonical before dispatch; determine which policy inputs the plan needs. */
    if (text_is_empty(selector)) {
        return CUP_ERR_INVALID_INPUT;
    }

    install_policy_init(&config);
    tool_preferences_init(&preferences);
    need_config = (registry_is_component(selector) && text_is_empty(value)) ||
                  strcmp(selector, "profile") == 0 || strcmp(selector, "toolchain") == 0;
    need_preferences =
        (registry_is_component(selector) && text_is_empty(value)) ||
        strcmp(selector, "profile") == 0;

    /* Build and fully validate the immutable plan from one shared state/catalog snapshot. */
    err = command_context_begin(&context, target_override, SYSTEM_LOCK_SHARED);
    if (err == CUP_OK) {
        err = command_context_load_state(&context);
    }
    if (err == CUP_OK) {
        err = command_context_load_catalog(&context);
    }
    if (err == CUP_OK && need_config) {
        err = install_policy_load(&config);
    }
    if (err == CUP_OK && need_preferences) {
        err = tool_preferences_load(&preferences);
    }
    if (err == CUP_OK) {
        err = install_plan_build(&plan,
                                 &config,
                                 &preferences,
                                 context.host_platform,
                                 context.target_platform,
                                 selector,
                                 value);
    }
    if (err == CUP_OK) {
        err = install_plan_validate(&plan, &context, format_override);
    }
    /* Release the shared preflight context before package transactions acquire exclusive locks. */
    command_context_end(&context);
    if (err != CUP_OK) {
        return err;
    }

    /* A single selection delegates directly; groups retain completed packages on later failure. */
    if (plan.kind == INSTALL_PLAN_SINGLE) {
        return package_install_artifact(&plan.items[0].artifact_spec);
    }

    printf("==> Installing %s '%s' (%zu package%s)...\n",
           plan.kind == INSTALL_PLAN_PROFILE ? "profile" : "toolchain",
           plan.description,
           plan.count,
           plan.count == 1 ? "" : "s");
    for (i = 0; i < plan.count; ++i) {
        InstallPlanItem *item = &plan.items[i];

        /* Revalidate each package under its exclusive context because the preflight state
         * snapshot may be stale by the time this group reaches the item. */
        err = package_install_artifact(&item->artifact_spec);
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            skipped_count++;
            continue;
        }
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Install group '%s' stopped while processing package %zu of %zu: "
                    "%zu installed, %zu skipped. Completed installations were retained.\n",
                    plan.description,
                    i + 1,
                    plan.count,
                    installed_count,
                    skipped_count);
            return err;
        }
        installed_count++;
    }

    printf("Install group '%s' completed: %zu package(s) installed, %zu skipped.\n",
           plan.description,
           installed_count,
           skipped_count);
    return CUP_OK;
}
