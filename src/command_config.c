/*
 * Shows and modifies scoped preferences for future installations without changing installed
 * package defaults in state.txt.
 */

#include "commands.h"

#include "command_context.h"
#include "install_policy.h"
#include "interrupt.h"
#include "registry.h"
#include "text.h"
#include "tool_preferences.h"

#include <stdio.h>
#include <string.h>

/* Render immutable profiles and curated toolchains after the scoped selection table. */
static void print_named_lists(const char *title, const InstallNamedList *lists, size_t count) {
    size_t i;

    printf("\n%s:\n", title);
    for (i = 0; i < count; ++i) {
        size_t item;

        printf("  %-12s ", lists[i].name);
        for (item = 0; item < lists[i].item_count; ++item) {
            printf("%s%s", item == 0 ? "" : ", ", lists[i].items[item]);
        }
        printf("\n");
    }
}

/* Resolve each component without mutating preferences or installed defaults. */
static CupError show_configuration(const InstallPolicy *policy,
                                   const ToolPreferences *preferences,
                                   const char *host,
                                   const char *target) {
    size_t i;

    printf("Install selections for host '%s', target '%s':\n\n", host, target);
    printf("%-18s %-18s %-18s %s\n", "Component", "Effective tool", "Official default", "Source");
    for (i = 0; i < registry_component_count(); ++i) {
        const char *component = registry_component_at(i);
        const InstallDefault *official;
        ToolPreferenceSource source = TOOL_PREFERENCE_NONE;
        char effective[MAX_IDENTIFIER_LEN] = "-";
        CupError err;

        if (component == NULL) {
            return CUP_ERR_INCONSISTENT_STATE;
        }
        official = install_policy_find_default(policy, host, target, component);
        err = tool_preferences_resolve(
            policy, preferences, host, target, component, effective, sizeof(effective), &source);
        if (err != CUP_OK && err != CUP_ERR_NOT_AVAILABLE) {
            return err;
        }
        printf("%-18s %-18s %-18s %s\n",
               component,
               source == TOOL_PREFERENCE_NONE ? "-" : effective,
               official == NULL ? "-" : official->tool,
               source == TOOL_PREFERENCE_USER               ? "user preference"
               : source == TOOL_PREFERENCE_OFFICIAL_DEFAULT ? "official default"
                                                            : "unavailable");
    }

    print_named_lists("Profiles", policy->profiles, policy->profile_count);
    print_named_lists("Toolchains", policy->toolchains, policy->toolchain_count);
    return CUP_OK;
}

static CupError set_preference(ToolPreferences *preferences,
                               const CommandContext *context,
                               const char *component,
                               const char *tool) {
    CupError err;

    err = tool_preferences_set(preferences,
                               context->host_platform,
                               context->target_platform,
                               component,
                               tool);
    if (err == CUP_OK) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK) {
        err = tool_preferences_save(preferences);
    }
    if (err == CUP_OK) {
        printf("Preferred tool for '%s' on target '%s' set to '%s'.\n",
               component,
               context->target_platform,
               tool);
    }
    return err;
}

static CupError reset_scope_preferences(ToolPreferences *preferences,
                                        const CommandContext *context) {
    size_t removed_count;
    CupError err;

    err = tool_preferences_reset_scope(preferences,
                                       context->host_platform,
                                       context->target_platform,
                                       &removed_count);
    if (err == CUP_OK && removed_count > 0) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK && removed_count > 0) {
        err = tool_preferences_save(preferences);
    }
    if (err == CUP_OK) {
        printf("Reset %zu preference(s) for target '%s'.\n",
               removed_count,
               context->target_platform);
    }
    return err;
}

static CupError reset_component_preference(ToolPreferences *preferences,
                                           const CommandContext *context,
                                           const char *component) {
    CupError err;
    int removed;

    err = tool_preferences_reset(preferences,
                                 context->host_platform,
                                 context->target_platform,
                                 component,
                                 &removed);
    if (err == CUP_OK && removed) {
        err = interrupt_safe_point();
    }
    if (err == CUP_OK && removed) {
        err = tool_preferences_save(preferences);
    }
    if (err == CUP_OK) {
        printf(removed ? "Preference for '%s' on target '%s' was reset.\n"
                       : "No preference was set for '%s' on target '%s'.\n",
               component,
               context->target_platform);
    }
    return err;
}

static CupError reset_preferences(ToolPreferences *preferences,
                                  const CommandContext *context,
                                  const char *component) {
    if (text_is_empty(component)) {
        return reset_scope_preferences(preferences, context);
    }
    return reset_component_preference(preferences, context, component);
}

/* View uses a read-only context; set/reset acquire the exclusive state-changing lock. */
CupError command_config(const char *action_input,
                        const char *name_input,
                        const char *value_input,
                        const char *target_override) {
    CommandContext context = {0};
    InstallPolicy policy;
    ToolPreferences preferences;
    CupError err;
    const char *action = action_input;
    const char *name = name_input;
    const char *value = value_input;
    int is_view;

    install_policy_init(&policy);
    tool_preferences_init(&preferences);
    is_view = text_is_empty(action);

    /* View needs official policy plus preferences. Mutations need only the private preference
     * document after the exclusive command context has established runtime ownership. */
    err = is_view ? command_context_begin_read_only(&context, target_override)
                  : command_context_begin(&context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK && is_view) {
        err = install_policy_load(&policy);
    }
    if (err == CUP_OK && (!is_view || context.runtime_available)) {
        err = tool_preferences_load(&preferences);
    }
    if (err != CUP_OK) {
        goto done;
    }

    if (is_view) {
        err = show_configuration(
            &policy, &preferences, context.host_platform, context.target_platform);
    } else if (strcmp(action, "set") == 0) {
        err = set_preference(&preferences, &context, name, value);
    } else if (strcmp(action, "reset") == 0) {
        err = reset_preferences(&preferences, &context, name);
    } else {
        fprintf(stderr, "Error: unknown config action '%s'.\n", action);
        err = CUP_ERR_INVALID_INPUT;
    }

done:
    command_context_end(&context);
    return err;
}
