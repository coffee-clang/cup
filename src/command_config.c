/*
 * Shows and modifies scoped preferences for future installations without changing installed
 * package defaults in state.txt.
 */

#include "commands.h"

#include "command_context.h"
#include "install_policy.h"
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
               effective,
               official == NULL ? "-" : official->tool,
               source == TOOL_PREFERENCE_USER               ? "user preference"
               : source == TOOL_PREFERENCE_OFFICIAL_DEFAULT ? "official default"
                                                            : "unavailable");
    }

    print_named_lists("Profiles", policy->profiles, policy->profile_count);
    print_named_lists("Toolchains", policy->toolchains, policy->toolchain_count);
    return CUP_OK;
}

static CupError set_preference(const InstallPolicy *policy,
                               ToolPreferences *preferences,
                               const CommandContext *context,
                               const char *component,
                               const char *tool) {
    CupError err;

    if (text_is_empty(component) || text_is_empty(tool)) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = tool_preferences_set(preferences,
                               context->host_platform,
                               context->target_platform,
                               component,
                               tool);
    if (err == CUP_OK) {
        err = tool_preferences_save(policy, preferences);
    }
    if (err == CUP_OK) {
        printf("Preferred tool for '%s' on target '%s' set to '%s'.\n",
               component,
               context->target_platform,
               tool);
    }
    return err;
}

static CupError reset_scope_preferences(const InstallPolicy *policy,
                                        ToolPreferences *preferences,
                                        const CommandContext *context) {
    size_t removed_count;
    CupError err;

    err = tool_preferences_reset_scope(preferences,
                                       context->host_platform,
                                       context->target_platform,
                                       &removed_count);
    if (err == CUP_OK) {
        err = tool_preferences_save(policy, preferences);
    }
    if (err == CUP_OK) {
        printf("Reset %zu preference(s) for target '%s'.\n",
               removed_count,
               context->target_platform);
    }
    return err;
}

static CupError reset_component_preference(const InstallPolicy *policy,
                                           ToolPreferences *preferences,
                                           const CommandContext *context,
                                           const char *component) {
    CupError err;
    int removed;

    if (registry_validate_component(component) != CUP_OK) {
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }

    err = tool_preferences_reset(preferences,
                                 context->host_platform,
                                 context->target_platform,
                                 component,
                                 &removed);
    if (err == CUP_OK) {
        err = tool_preferences_save(policy, preferences);
    }
    if (err == CUP_OK) {
        printf(removed ? "Preference for '%s' on target '%s' was reset.\n"
                       : "No preference was set for '%s' on target '%s'.\n",
               component,
               context->target_platform);
    }
    return err;
}

static CupError reset_preferences(const InstallPolicy *policy,
                                  ToolPreferences *preferences,
                                  const CommandContext *context,
                                  const char *component,
                                  const char *unexpected_value) {
    if (!text_is_empty(unexpected_value)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (text_is_empty(component)) {
        return reset_scope_preferences(policy, preferences, context);
    }
    return reset_component_preference(policy, preferences, context, component);
}

/* View uses a read-only context; set/reset acquire the exclusive mutation lock. */
CupError command_config(const char *action_input,
                        const char *name_input,
                        const char *value_input,
                        const char *target_override) {
    CommandContext context = {0};
    InstallPolicy policy;
    ToolPreferences preferences;
    CupError err;
    char action[MAX_IDENTIFIER_LEN] = "";
    char name[MAX_IDENTIFIER_LEN] = "";
    char value[MAX_IDENTIFIER_LEN] = "";
    int is_view = text_is_empty(action_input);

    install_policy_init(&policy);
    tool_preferences_init(&preferences);
    if (!is_view && (text_copy_lower_ascii(action, sizeof(action), action_input) != CUP_OK ||
                     (!text_is_empty(name_input) &&
                      text_copy_lower_ascii(name, sizeof(name), name_input) != CUP_OK) ||
                     (!text_is_empty(value_input) &&
                      text_copy_lower_ascii(value, sizeof(value), value_input) != CUP_OK))) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    /* Load policy and preferences under the least permissive context required by the action. */
    err = is_view ? command_context_begin_read_only(&context, target_override)
                  : command_context_begin(&context, target_override, SYSTEM_LOCK_EXCLUSIVE);
    if (err == CUP_OK) {
        err = install_policy_load(&policy);
    }
    if (err == CUP_OK) {
        err = tool_preferences_load(&policy, &preferences);
    }
    if (err != CUP_OK) {
        goto done;
    }

    if (is_view) {
        err = show_configuration(
            &policy, &preferences, context.host_platform, context.target_platform);
    } else if (strcmp(action, "set") == 0) {
        err = set_preference(&policy, &preferences, &context, name, value);
    } else if (strcmp(action, "reset") == 0) {
        err = reset_preferences(&policy, &preferences, &context, name, value);
    } else {
        fprintf(stderr, "Error: unknown config action '%s'.\n", action);
        err = CUP_ERR_INVALID_INPUT;
    }

done:
    command_context_end(&context);
    return err;
}
