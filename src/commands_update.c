#include "commands.h"

#include "command_context.h"
#include "entry.h"
#include "registry.h"
#include "state.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char target[MAX_PLATFORM_LEN];
    char previous_default[MAX_ENTRY_LEN];
} UpdatePlanItem;

typedef struct {
    UpdatePlanItem items[MAX_INSTALLED];
    size_t count;
} UpdatePlan;

static int plan_find(const UpdatePlan *plan, const char *component,
    const char *tool, const char *target) {
    size_t i;

    for (i = 0; i < plan->count; ++i) {
        if (strcmp(plan->items[i].component, component) == 0 &&
            strcmp(plan->items[i].tool, tool) == 0 &&
            strcmp(plan->items[i].target, target) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static CupError plan_add(UpdatePlan *plan, const CommandContext *context,
    const StateEntry *installed) {
    UpdatePlanItem *item;
    const char *default_entry;
    char tool[MAX_NAME_LEN];
    char version[MAX_NAME_LEN];
    char default_tool[MAX_NAME_LEN];
    char default_version[MAX_NAME_LEN];

    if (entry_parse(installed->entry, tool, sizeof(tool),
            version, sizeof(version)) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    if (plan_find(plan, installed->component, tool,
            installed->target_platform) >= 0) {
        return CUP_OK;
    }
    if (plan->count >= MAX_INSTALLED) {
        return CUP_ERR_STATE_FULL;
    }

    item = &plan->items[plan->count++];
    memset(item, 0, sizeof(*item));
    if (text_copy(item->component, sizeof(item->component),
            installed->component) != CUP_OK ||
        text_copy(item->tool, sizeof(item->tool), tool) != CUP_OK ||
        text_copy(item->target, sizeof(item->target),
            installed->target_platform) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    default_entry = state_get_default(&context->state, item->component,
        context->host_platform, item->target);
    if (default_entry != NULL &&
        entry_parse(default_entry, default_tool, sizeof(default_tool),
            default_version, sizeof(default_version)) == CUP_OK &&
        strcmp(default_tool, item->tool) == 0 &&
        text_copy(item->previous_default, sizeof(item->previous_default),
            default_entry) != CUP_OK) {
        return CUP_ERR_BUFFER_TOO_SMALL;
    }

    return CUP_OK;
}

static CupError build_update_plan(const char *name, UpdatePlan *plan) {
    CommandContext context = {0};
    CupError err;
    char requested_component[MAX_NAME_LEN] = "";
    char requested_tool[MAX_NAME_LEN] = "";
    int component_request;
    size_t i;

    memset(plan, 0, sizeof(*plan));
    component_request = registry_is_component(name);
    if (component_request) {
        err = text_copy(requested_component, sizeof(requested_component), name);
    } else {
        err = registry_find_tool_component(name, requested_component,
            sizeof(requested_component));
        if (err == CUP_OK) {
            err = text_copy(requested_tool, sizeof(requested_tool), name);
        }
    }
    if (err != CUP_OK) {
        return err;
    }

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_SHARED);
    if (err != CUP_OK) {
        goto done;
    }
    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    for (i = 0; i < context.state.installed_count; ++i) {
        const StateEntry *installed = &context.state.installed[i];
        char tool[MAX_NAME_LEN];
        char version[MAX_NAME_LEN];

        if (strcmp(installed->host_platform, context.host_platform) != 0 ||
            strcmp(installed->component, requested_component) != 0 ||
            entry_parse(installed->entry, tool, sizeof(tool),
                version, sizeof(version)) != CUP_OK ||
            (!component_request && strcmp(tool, requested_tool) != 0)) {
            continue;
        }

        err = plan_add(plan, &context, installed);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = CUP_OK;

done:
    command_context_end(&context);
    return err;
}

CupError command_update(const char *name) {
    UpdatePlan plan;
    CupError err;
    size_t i;
    size_t installed_count = 0;
    size_t default_count = 0;

    if (name == NULL || name[0] == '\0') {
        return CUP_ERR_INVALID_INPUT;
    }

    err = build_update_plan(name, &plan);
    if (err != CUP_OK) {
        return err;
    }
    if (plan.count == 0) {
        printf("No installed scopes match '%s'.\n", name);
        return CUP_OK;
    }

    for (i = 0; i < plan.count; ++i) {
        UpdatePlanItem *item = &plan.items[i];
        int installed = 0;
        int default_moved = 0;

        printf("==> Updating %s:%s for target '%s'...\n",
            item->component, item->tool, item->target);
        err = command_update_scope(item->component, item->tool,
            item->target, item->previous_default,
            &installed, &default_moved);
        if (err != CUP_OK) {
            return err;
        }
        installed_count += (size_t)installed;
        default_count += (size_t)default_moved;
    }

    printf("Update completed for '%s': %zu stable package(s) installed, "
        "%zu default(s) moved; previous releases were retained.\n",
        name, installed_count, default_count);
    return CUP_OK;
}
