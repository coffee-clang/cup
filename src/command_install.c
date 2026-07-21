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
    char resolved_selector[MAX_SELECTOR_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char format[MAX_IDENTIFIER_LEN];
    int already_installed;
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

/* Request normalization. CLI forms are reduced to one component/selector representation before
 * policy resolution begins. */
static CupError normalize_entry(const char *input, char *selector, size_t size) {
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
    const char *separator;
    CupError err;

    if (input == NULL || selector == NULL || size == 0 || text_is_empty(input)) {
        return CUP_ERR_INVALID_INPUT;
    }
    separator = strchr(input, '@');
    if (separator == NULL) {
        err = text_copy_lower_ascii(tool, sizeof(tool), input);
        if (err != CUP_OK) {
            return err;
        }
        return package_selector_format_parts(selector, size, tool, "stable");
    }
    err = package_selector_parse_parts(input, tool, sizeof(tool), release, sizeof(release));
    if (err != CUP_OK) {
        return err;
    }
    err = text_copy_lower_ascii(tool, sizeof(tool), tool);
    if (err == CUP_OK) {
        char normalized_release[MAX_IDENTIFIER_LEN];

        err = text_copy_lower_ascii(normalized_release, sizeof(normalized_release), release);
        if (err == CUP_OK && strcmp(normalized_release, "stable") == 0) {
            err = text_copy(release, sizeof(release), "stable");
        }
    }
    if (err != CUP_OK) {
        return err;
    }
    return package_selector_format_parts(selector, size, tool, release);
}

static CupError plan_add(InstallPlan *plan, const char *component, const char *selector) {
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

static CupError add_component_selection(InstallPlan *plan,
                                        const InstallPolicy *config,
                                        const ToolPreferences *preferences,
                                        const char *host_platform,
                                        const char *target_platform,
                                        const char *component,
                                        const char *explicit_entry) {
    char selector[MAX_SELECTOR_LEN];
    CupError err;

    if (!text_is_empty(explicit_entry)) {
        err = normalize_entry(explicit_entry, selector, sizeof(selector));
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
    return err == CUP_OK ? plan_add(plan, component, selector) : err;
}

/* Plan construction. Profiles and toolchains expand into a bounded list before any package is
 * downloaded. */
static CupError build_plan(InstallPlan *plan,
                           const InstallPolicy *config,
                           const ToolPreferences *preferences,
                           const char *host_platform,
                           const char *target_platform,
                           const char *selector_input,
                           const char *value_input) {
    char selector[MAX_IDENTIFIER_LEN];
    char value[MAX_SELECTOR_LEN] = "";
    CupError err;
    size_t i;

    memset(plan, 0, sizeof(*plan));
    if (text_copy_lower_ascii(selector, sizeof(selector), selector_input) != CUP_OK ||
        (!text_is_empty(value_input) && text_copy(value, sizeof(value), value_input) != CUP_OK)) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    if (registry_is_component(selector)) {
        plan->kind = INSTALL_PLAN_SINGLE;
        if (text_copy(plan->description, sizeof(plan->description), selector) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        return add_component_selection(plan,
                                       config,
                                       preferences,
                                       host_platform,
                                       target_platform,
                                       selector,
                                       text_is_empty(value) ? NULL : value);
    }

    if (strcmp(selector, "profile") == 0) {
        const InstallNamedList *profile;

        if (text_is_empty(value)) {
            fprintf(stderr, "Error: install profile requires a profile name.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        err = text_copy_lower_ascii(value, sizeof(value), value);
        if (err != CUP_OK) {
            return err;
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
            err = add_component_selection(
                plan, config, preferences, host_platform, target_platform, profile->items[i], NULL);
            if (err != CUP_OK) {
                return err;
            }
        }
        return CUP_OK;
    }

    if (strcmp(selector, "toolchain") == 0) {
        const InstallNamedList *toolchain;

        if (text_is_empty(value)) {
            fprintf(stderr, "Error: install toolchain requires a toolchain name.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        err = text_copy_lower_ascii(value, sizeof(value), value);
        if (err != CUP_OK) {
            return err;
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
            char selector[MAX_SELECTOR_LEN];

            err = registry_find_tool_component(toolchain->items[i], component, sizeof(component));
            if (err == CUP_OK) {
                err = package_selector_format_parts(
                    selector, sizeof(selector), toolchain->items[i], "stable");
            }
            if (err == CUP_OK) {
                err = plan_add(plan, component, selector);
            }
            if (err != CUP_OK) {
                return err;
            }
        }
        return CUP_OK;
    }

    fprintf(stderr, "Error: unsupported component or install group '%s'.\n", selector);
    return CUP_ERR_UNSUPPORTED_COMPONENT;
}

/* Full preflight. Every catalog selection and installed-package condition is validated before the
 * first side effect. */
static CupError validate_plan(InstallPlan *plan,
                              CommandContext *context,
                              const char *format_override) {
    CupError err;
    size_t i;
    size_t unavailable_count = 0;

    for (i = 0; i < plan->count; ++i) {
        InstallPlanItem *item = &plan->items[i];
        PackageRequest request;
        int package_available;
        int version_available;

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
            unavailable_count++;
            continue;
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
            unavailable_count++;
            continue;
        }
        if (!text_is_empty(format_override)) {
            int format_available;
            err = package_catalog_has_format(&context->catalog,
                                             item->component,
                                             request.selector.tool,
                                             context->host_platform,
                                             context->target_platform,
                                             format_override,
                                             &format_available);
            if (err != CUP_OK) {
                return err;
            }
            if (!format_available) {
                item->available = 0;
                unavailable_count++;
                continue;
            }
            err = text_copy(item->format, sizeof(item->format), format_override);
        } else {
            err = package_catalog_get_default_format(&context->catalog,
                                                     item->format,
                                                     sizeof(item->format),
                                                     item->component,
                                                     request.selector.tool,
                                                     context->host_platform,
                                                     context->target_platform);
        }
        if (err != CUP_OK) {
            return err;
        }

        item->available = 1;
        if (text_copy(item->resolved_selector,
                      sizeof(item->resolved_selector),
                      request.resolved_selector) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        {
            PackageIdentity identity;

            err = package_identity_from_selector(&identity,
                                                 item->component,
                                                 context->host_platform,
                                                 context->target_platform,
                                                 item->resolved_selector);
            if (err != CUP_OK) {
                return err;
            }
            item->already_installed = state_find_installed(&context->state, &identity) >= 0;
            if (item->already_installed) {
                err = installed_package_require_valid(&context->state, &identity);
                if (err != CUP_OK) {
                    fprintf(stderr,
                            "Error: selected package '%s:%s' is recorded but invalid; "
                            "run 'cup doctor' and 'cup repair'.\n",
                            item->component,
                            item->resolved_selector);
                    return err;
                }
            }
        }
    }

    if (unavailable_count != 0) {
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
        return CUP_ERR_NOT_AVAILABLE;
    }
    return CUP_OK;
}

/* Public command entry points. Execution delegates each validated item to the reusable package
 * installation transaction. */
CupError command_install(const char *component,
                         const char *selector,
                         const char *target_override,
                         const char *format_override) {
    return package_install(component, selector, target_override, format_override);
}

CupError command_install_request(const char *selector,
                                 const char *value,
                                 const char *target_override,
                                 const char *format_override) {
    CommandContext context = {0};
    InstallPolicy config;
    ToolPreferences preferences;
    InstallPlan plan;
    CupError err;
    char normalized_selector[MAX_IDENTIFIER_LEN];
    char normalized_format[MAX_IDENTIFIER_LEN] = "";
    int need_config;
    int need_preferences;
    size_t i;
    size_t installed_count = 0;
    size_t skipped_count = 0;

    if (text_is_empty(selector)) {
        return CUP_ERR_INVALID_INPUT;
    }

    install_policy_init(&config);
    tool_preferences_init(&preferences);
    err = text_copy_lower_ascii(normalized_selector, sizeof(normalized_selector), selector);
    if (err == CUP_OK && !text_is_empty(format_override)) {
        err = text_copy_lower_ascii(normalized_format, sizeof(normalized_format), format_override);
    }
    if (err != CUP_OK) {
        return err;
    }
    need_config = (registry_is_component(normalized_selector) && text_is_empty(value)) ||
                  strcmp(normalized_selector, "profile") == 0 ||
                  strcmp(normalized_selector, "toolchain") == 0;
    need_preferences = (registry_is_component(normalized_selector) && text_is_empty(value)) ||
                       strcmp(normalized_selector, "profile") == 0;

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
        err = tool_preferences_load(&config, &preferences);
    }
    if (err == CUP_OK) {
        err = build_plan(&plan,
                         &config,
                         &preferences,
                         context.host_platform,
                         context.target_platform,
                         selector,
                         value);
    }
    if (err == CUP_OK) {
        err = validate_plan(
            &plan, &context, text_is_empty(normalized_format) ? NULL : normalized_format);
    }
    command_context_end(&context);
    if (err != CUP_OK) {
        return err;
    }

    if (plan.kind == INSTALL_PLAN_SINGLE) {
        return package_install(plan.items[0].component,
                               plan.items[0].resolved_selector,
                               target_override,
                               plan.items[0].format);
    }

    printf("==> Installing %s '%s' (%zu package%s)...\n",
           plan.kind == INSTALL_PLAN_PROFILE ? "profile" : "toolchain",
           plan.description,
           plan.count,
           plan.count == 1 ? "" : "s");
    for (i = 0; i < plan.count; ++i) {
        InstallPlanItem *item = &plan.items[i];

        if (item->already_installed) {
            printf("==> Skipping %s:%s; the selected stable package is already installed.\n",
                   item->component,
                   item->tool);
            skipped_count++;
            continue;
        }
        err = package_install(
            item->component, item->resolved_selector, target_override, item->format);
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
            if (err == CUP_ERR_COMMIT) {
                fprintf(stderr,
                        "The current package may also have committed; run 'cup repair' "
                        "before continuing.\n");
            }
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
