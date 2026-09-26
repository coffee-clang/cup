/* Resolve public install input into a validated plan. Profiles use preferences; curated
 * toolchains remain explicit. */

#include "commands.h"

#include "catalog_refresh.h"
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
#include "ui.h"

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
    char description[MAX_SELECTOR_LEN];
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
        const ToolPreference *preference =
            tool_preferences_find(preferences, target_platform, component);
        const InstallDefault *official = NULL;
        const char *tool;

        if (preference != NULL) {
            tool = preference->tool;
        } else {
            official = install_policy_find_default(config, host_platform, target_platform, component);
            if (official == NULL) {
                return CUP_ERR_NOT_AVAILABLE;
            }
            tool = official->tool;
        }
        err = package_selector_format_parts(selector, sizeof(selector), tool, "stable");
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
static CupError install_plan_resolve_format(const InstallPlanItem *item,
                                            const PackageRequest *request,
                                            const CommandContext *context,
                                            const char *format_override,
                                            char *format,
                                            size_t format_size) {
    if (!text_is_empty(format_override)) {
        return text_copy(format, format_size, format_override);
    }

    {
        PackageArchiveFormat default_format;
        CupError err = package_archive_default_format(context->host_platform, &default_format);

        (void)item;
        (void)request;
        return err == CUP_OK
                   ? text_copy(format, format_size, package_archive_format_name(default_format))
                   : err;
    }
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
    err = install_plan_resolve_format(
        item, &request, context, format_override, format, sizeof(format));
    if (err != CUP_OK) {
        return err;
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
        if (err == CUP_ERR_NOT_AVAILABLE) {
            item->available = 0;
            *unavailable = 1;
            return CUP_OK;
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
                                      const char *format_override,
                                      int report_unavailable) {
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
        if (report_unavailable) {
            install_plan_print_unavailable(plan, context);
        }
        return CUP_ERR_NOT_AVAILABLE;
    }
    return CUP_OK;
}

static CupError install_plan_has_symbolic(const InstallPlan *plan, int *has_symbolic) {
    size_t i;

    if (plan == NULL || has_symbolic == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *has_symbolic = 0;
    for (i = 0; i < plan->count; ++i) {
        PackageRequest request;
        CupError err = package_request_parse(plan->items[i].component,
                                             plan->items[i].selector,
                                             &request);
        if (err != CUP_OK) {
            return err;
        }
        if (package_release_is_stable(request.selector.release)) {
            *has_symbolic = 1;
            return CUP_OK;
        }
    }
    return CUP_OK;
}

/* Exact installed requests are fully local: state + package integrity are enough for a no-op. */
static CupError install_plan_exact_local_noop(const InstallPlan *plan,
                                              const CommandContext *context,
                                              int *handled) {
    PackageRequest request;
    PackageIdentity identity;
    CupError err;

    if (plan == NULL || context == NULL || handled == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *handled = 0;
    if (plan->kind != INSTALL_PLAN_SINGLE || plan->count != 1) {
        return CUP_OK;
    }
    err = package_request_parse(plan->items[0].component, plan->items[0].selector, &request);
    if (err != CUP_OK) {
        return err;
    }
    if (package_release_is_stable(request.selector.release)) {
        return CUP_OK;
    }
    err = package_identity_init(&identity,
                                plan->items[0].component,
                                request.selector.tool,
                                context->host_platform,
                                context->target_platform,
                                request.selector.release);
    if (err != CUP_OK) {
        return err;
    }
    if (state_find_installed(&context->state, &identity) < 0) {
        return CUP_OK;
    }
    err = installed_package_require_valid(&context->state, &identity);
    if (err != CUP_OK) {
        return err;
    }
    printf("%s@%s is already installed for target '%s'; no changes were made.\n",
           identity.tool,
           identity.version,
           identity.target_platform);
    *handled = 1;
    return CUP_ERR_ALREADY_INSTALLED;
}

static CupError install_plan_open_snapshot(CommandContext *context,
                                           InstallPolicy *config,
                                           ToolPreferences *preferences,
                                           InstallPlan *plan,
                                           const char *selector,
                                           const char *value,
                                           const char *target_override,
                                           int need_config,
                                           int need_preferences,
                                           int load_catalog) {
    CupError err;

    err = command_context_begin_initialize(context, target_override, SYSTEM_LOCK_SHARED);
    if (err == CUP_OK) {
        err = command_context_load_state(context);
    }
    if (err == CUP_OK && need_config) {
        err = install_policy_load(config);
    }
    if (err == CUP_OK && need_preferences) {
        err = tool_preferences_load(preferences, stderr);
    }
    if (err == CUP_OK) {
        err = install_plan_build(plan,
                                 need_config ? config : NULL,
                                 need_preferences ? preferences : NULL,
                                 context->host_platform,
                                 context->target_platform,
                                 selector,
                                 value);
    }
    if (err == CUP_OK && load_catalog) {
        err = command_context_load_catalog(context);
    }
    return err;
}

/* Public request planning delegates catalog-pinned artifacts to package installation. */
CupError command_install(const char *selector,
                         const char *value,
                         const char *target_override,
                         const char *format_override) {
    CommandContext context;
    InstallPolicy config;
    ToolPreferences preferences;
    InstallPlan plan;
    CupError err;
    CupError refresh_err = CUP_OK;
    int need_config;
    int need_preferences;
    int has_symbolic = 0;
    int handled = 0;
    int refreshed = 0;
    size_t i;
    size_t installed_count = 0;
    size_t skipped_count = 0;

    if (text_is_empty(selector)) {
        return CUP_ERR_INVALID_INPUT;
    }

    need_config = (registry_is_component(selector) && text_is_empty(value)) ||
                  strcmp(selector, "profile") == 0 || strcmp(selector, "toolchain") == 0;
    need_preferences =
        (registry_is_component(selector) && text_is_empty(value)) ||
        strcmp(selector, "profile") == 0;

    /* Build a local plan first. Exact installed identities need no catalog or network at all. */
    err = install_plan_open_snapshot(&context,
                                     &config,
                                     &preferences,
                                     &plan,
                                     selector,
                                     value,
                                     target_override,
                                     need_config,
                                     need_preferences,
                                     0);
    if (err == CUP_OK) {
        err = install_plan_exact_local_noop(&plan, &context, &handled);
    }
    if (handled || err != CUP_OK) {
        command_context_end(&context);
        return err;
    }
    err = install_plan_has_symbolic(&plan, &has_symbolic);
    if (err != CUP_OK) {
        command_context_end(&context);
        return err;
    }

    /* A usable local catalog is the refresh authority and may already satisfy an exact request. */
    err = command_context_load_catalog(&context);
    if (err != CUP_OK) {
        command_context_end(&context);
        return err;
    }

    if (!has_symbolic) {
        err = install_plan_validate(&plan, &context, format_override, 0);
        if (err == CUP_OK) {
            command_context_end(&context);
            goto execute_plan;
        }
        if (err != CUP_ERR_NOT_AVAILABLE) {
            command_context_end(&context);
            return err;
        }
    }

    /* Network is never performed while the shared planning snapshot is locked. */
    command_context_end(&context);
    ui_phase("Refreshing catalog...");
    refresh_err = catalog_refresh_existing(
        &refreshed, has_symbolic ? CATALOG_REFRESH_QUIET : CATALOG_REFRESH_REPORT_ERRORS);
    if (!has_symbolic && refresh_err != CUP_OK) {
        return refresh_err;
    }

    /* Rebuild everything after the network window; no state/preference snapshot crosses it. */
    err = install_plan_open_snapshot(&context,
                                     &config,
                                     &preferences,
                                     &plan,
                                     selector,
                                     value,
                                     target_override,
                                     need_config,
                                     need_preferences,
                                     1);
    if (err == CUP_OK) {
        err = install_plan_validate(&plan, &context, format_override, 1);
    }
    command_context_end(&context);
    if (err != CUP_OK) {
        return refresh_err != CUP_OK ? refresh_err : err;
    }
    if (has_symbolic && refresh_err != CUP_OK) {
        fprintf(stderr,
                "Warning: catalog refresh failed; installing from the valid local catalog.\n");
    }

execute_plan:
    if (plan.kind == INSTALL_PLAN_SINGLE) {
        return package_install_artifact(&plan.items[0].artifact_spec);
    }

    ui_phase("Installing %s '%s' (%zu package%s)...",
             plan.kind == INSTALL_PLAN_PROFILE ? "profile" : "toolchain",
             plan.description,
             plan.count,
             plan.count == 1 ? "" : "s");
    for (i = 0; i < plan.count; ++i) {
        InstallPlanItem *item = &plan.items[i];

        err = package_install_artifact(&item->artifact_spec);
        if (err == CUP_ERR_ALREADY_INSTALLED) {
            skipped_count++;
            continue;
        }
        if (err != CUP_OK) {
            fprintf(stderr,
                    "Installation of '%s' stopped at package %zu of %zu: "
                    "%zu installed, %zu skipped. Completed installations were kept.\n",
                    plan.description,
                    i + 1,
                    plan.count,
                    installed_count,
                    skipped_count);
            return err;
        }
        installed_count++;
    }

    printf("Installed %s '%s': %zu installed, %zu skipped.\n",
           plan.kind == INSTALL_PLAN_PROFILE ? "profile" : "toolchain",
           plan.description,
           installed_count,
           skipped_count);
    return CUP_OK;
}
