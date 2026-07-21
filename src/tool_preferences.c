/*
 * Loads, validates and atomically persists scoped preferred tools for future installations.
 */

#include "tool_preferences.h"

#include "layout.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_PREFERENCES_FORMAT "1"

/* Preference lookup and validation. Every override belongs to one complete package scope. */
static int preference_index(const ToolPreferences *preferences, const PackageScope *scope) {
    size_t i;

    if (preferences == NULL || scope == NULL) {
        return -1;
    }
    for (i = 0; i < preferences->count; ++i) {
        if (package_scope_equals(&preferences->items[i].scope, scope)) {
            return (int)i;
        }
    }
    return -1;
}

static CupError validate_preferences(const ToolPreferences *preferences) {
    size_t i;

    if (preferences == NULL || preferences->count > MAX_INSTALL_DEFAULTS) {
        return CUP_ERR_INVALID_INPUT;
    }
    for (i = 0; i < preferences->count; ++i) {
        const ToolPreference *entry = &preferences->items[i];
        size_t previous;

        if (package_scope_init(&(PackageScope){0},
                               entry->scope.component,
                               entry->scope.host_platform,
                               entry->scope.target_platform) != CUP_OK ||
            registry_validate_tool(entry->scope.component, entry->tool) != CUP_OK) {
            return CUP_ERR_VALIDATION;
        }
        for (previous = 0; previous < i; ++previous) {
            if (package_scope_equals(&entry->scope, &preferences->items[previous].scope)) {
                return CUP_ERR_VALIDATION;
            }
        }
    }
    return CUP_OK;
}

void tool_preferences_init(ToolPreferences *preferences) {
    if (preferences != NULL) {
        memset(preferences, 0, sizeof(*preferences));
    }
}

static CupError parse_preference_key(char *key, PackageScope *scope) {
    char prefix[MAX_IDENTIFIER_LEN];
    char host[MAX_PLATFORM_LEN];
    char target[MAX_PLATFORM_LEN];
    char component[MAX_IDENTIFIER_LEN];
    TextBuffer parts[4];

    parts[0] = (TextBuffer){prefix, sizeof(prefix)};
    parts[1] = (TextBuffer){host, sizeof(host)};
    parts[2] = (TextBuffer){target, sizeof(target)};
    parts[3] = (TextBuffer){component, sizeof(component)};
    if (text_split_exact(key, '.', parts, 4) != CUP_OK || strcmp(prefix, "preferred") != 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    return package_scope_init(scope, component, host, target);
}

/* Strict preferences.txt loading. Invalid records never produce a partially usable preference set.
 */
CupError tool_preferences_load(const InstallPolicy *policy, ToolPreferences *preferences) {
    char path[MAX_PATH_LEN];
    char line[MAX_INSTALL_POLICY_LINE_LEN];
    FILE *file;
    CupError err;
    size_t line_number = 0;
    int has_line;
    int exists;
    int format_seen = 0;

    if (policy == NULL || preferences == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    tool_preferences_init(preferences);

    /* A missing file means no overrides; a present file must be parsed completely and strictly. */
    err = layout_get_preferences_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

    /* Parse format and scoped preferences without exposing a partially loaded result. */
    while (1) {
        char key[MAX_CATALOG_KEY_LEN];
        char value[MAX_CATALOG_VALUE_LEN];

        err = text_read_line(file, line, sizeof(line), &has_line, &line_number);
        if (err != CUP_OK) {
            goto invalid;
        }
        if (!has_line) {
            break;
        }
        if (text_parse_key_value(line, key, sizeof(key), value, sizeof(value)) != CUP_OK) {
            err = CUP_ERR_VALIDATION;
            goto invalid;
        }
        if (strcmp(key, "format") == 0) {
            if (format_seen || strcmp(value, TOOL_PREFERENCES_FORMAT) != 0) {
                err = CUP_ERR_VALIDATION;
                goto invalid;
            }
            format_seen = 1;
        } else {
            ToolPreference *entry;
            PackageScope scope;

            if (parse_preference_key(key, &scope) != CUP_OK ||
                registry_validate_tool(scope.component, value) != CUP_OK ||
                preference_index(preferences, &scope) >= 0 ||
                preferences->count >= MAX_INSTALL_DEFAULTS) {
                err = CUP_ERR_VALIDATION;
                goto invalid;
            }
            entry = &preferences->items[preferences->count++];
            memset(entry, 0, sizeof(*entry));
            entry->scope = scope;
            if (text_copy(entry->tool, sizeof(entry->tool), value) != CUP_OK) {
                err = CUP_ERR_BUFFER_TOO_SMALL;
                goto invalid;
            }
        }
    }

    /* Validate the complete set only after the physical file has been consumed. */
    if (fclose(file) != 0) {
        tool_preferences_init(preferences);
        return CUP_ERR_FILESYSTEM;
    }
    if (!format_seen || validate_preferences(preferences) != CUP_OK) {
        tool_preferences_init(preferences);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;

invalid:
    fprintf(stderr, "Error: invalid user preferences line %zu.\n", line_number);
    fclose(file);
    tool_preferences_init(preferences);
    return err == CUP_ERR_FILESYSTEM ? err : CUP_ERR_VALIDATION;
}

/* Canonical persistence. Sorting makes the file stable regardless of the order in which preferences
 * were changed. */
static int compare_preferences(const void *left_value, const void *right_value) {
    const ToolPreference *left = left_value;
    const ToolPreference *right = right_value;
    int result;

    result = strcmp(left->scope.host_platform, right->scope.host_platform);
    if (result == 0) {
        result = strcmp(left->scope.target_platform, right->scope.target_platform);
    }
    if (result == 0) {
        result = strcmp(left->scope.component, right->scope.component);
    }
    return result;
}

CupError tool_preferences_save(const InstallPolicy *policy, const ToolPreferences *preferences) {
    ToolPreference sorted[MAX_INSTALL_DEFAULTS];
    char directory[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    char temporary[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;
    SystemCommitState commit_state = SYSTEM_COMMIT_NOT_APPLIED;
    size_t i;

    if (policy == NULL || validate_preferences(preferences) != CUP_OK) {
        return CUP_ERR_VALIDATION;
    }
    if (preferences->count == 0) {
        return tool_preferences_reset_all();
    }
    memcpy(sorted, preferences->items, preferences->count * sizeof(preferences->items[0]));
    qsort(sorted, preferences->count, sizeof(sorted[0]), compare_preferences);

    err = layout_ensure_config();
    if (err != CUP_OK) {
        return err;
    }
    err = layout_get_config_dir(directory, sizeof(directory));
    if (err == CUP_OK) {
        err = layout_get_preferences_path(path, sizeof(path));
    }
    if (err == CUP_OK) {
        err =
            system_create_temp_file(directory, "preferences", temporary, sizeof(temporary), &file);
    }
    if (err != CUP_OK) {
        return err;
    }

    if (fprintf(file, "format=%s\n", TOOL_PREFERENCES_FORMAT) < 0) {
        err = CUP_ERR_FILESYSTEM;
        goto write_failed;
    }
    for (i = 0; i < preferences->count; ++i) {
        const ToolPreference *entry = &sorted[i];

        if (fprintf(file,
                    "preferred.%s.%s.%s=%s\n",
                    entry->scope.host_platform,
                    entry->scope.target_platform,
                    entry->scope.component,
                    entry->tool) < 0) {
            err = CUP_ERR_FILESYSTEM;
            goto write_failed;
        }
    }
    err = system_sync_file(file);
    if (fclose(file) != 0 && err == CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
    }
    file = NULL;
    if (err != CUP_OK) {
        system_remove_file(temporary);
        return err;
    }

    err = system_replace_file(temporary, path, &commit_state);
    if (err != CUP_OK && commit_state == SYSTEM_COMMIT_NOT_APPLIED) {
        system_remove_file(temporary);
    }
    return commit_state == SYSTEM_COMMIT_APPLIED ? CUP_ERR_COMMIT : err;

write_failed:
    fclose(file);
    system_remove_file(temporary);
    return err;
}

CupError tool_preferences_reset_all(void) {
    char path[MAX_PATH_LEN];
    CupError err;
    int exists;

    err = layout_get_preferences_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(path, &exists);
    if (err != CUP_OK || !exists) {
        return err;
    }
    err = system_remove_file(path);
    if (err != CUP_OK) {
        return err;
    }
    return system_sync_parent_directory(path);
}

/* Scoped mutations and resolution. Changes are saved atomically, and resolution preserves CLI >
 * user > official precedence. */
CupError tool_preferences_set(ToolPreferences *preferences,
                              const char *host_platform,
                              const char *target_platform,
                              const char *component,
                              const char *tool) {
    PackageScope scope;
    ToolPreference *entry;
    int index;
    CupError err;

    if (preferences == NULL ||
        package_scope_init(&scope, component, host_platform, target_platform) != CUP_OK ||
        registry_validate_tool(component, tool) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    index = preference_index(preferences, &scope);
    if (index < 0) {
        if (preferences->count >= MAX_INSTALL_DEFAULTS) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        entry = &preferences->items[preferences->count++];
        memset(entry, 0, sizeof(*entry));
        entry->scope = scope;
    } else {
        entry = &preferences->items[index];
    }
    err = text_copy(entry->tool, sizeof(entry->tool), tool);
    return err;
}

CupError tool_preferences_reset(ToolPreferences *preferences,
                                const char *host_platform,
                                const char *target_platform,
                                const char *component,
                                int *removed) {
    PackageScope scope;
    int index;
    size_t i;

    if (preferences == NULL || removed == NULL ||
        package_scope_init(&scope, component, host_platform, target_platform) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    *removed = 0;
    index = preference_index(preferences, &scope);
    if (index < 0) {
        return CUP_OK;
    }
    for (i = (size_t)index + 1; i < preferences->count; ++i) {
        preferences->items[i - 1] = preferences->items[i];
    }
    preferences->count--;
    memset(
        &preferences->items[preferences->count], 0, sizeof(preferences->items[preferences->count]));
    *removed = 1;
    return CUP_OK;
}

CupError tool_preferences_reset_scope(ToolPreferences *preferences,
                                      const char *host_platform,
                                      const char *target_platform,
                                      size_t *removed_count) {
    size_t read_index;
    size_t write_index = 0;

    if (preferences == NULL || removed_count == NULL ||
        package_scope_init(&(PackageScope){0}, "compiler", host_platform, target_platform) !=
            CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }
    *removed_count = 0;
    for (read_index = 0; read_index < preferences->count; ++read_index) {
        const ToolPreference *entry = &preferences->items[read_index];

        if (strcmp(entry->scope.host_platform, host_platform) == 0 &&
            strcmp(entry->scope.target_platform, target_platform) == 0) {
            (*removed_count)++;
            continue;
        }
        if (write_index != read_index) {
            preferences->items[write_index] = preferences->items[read_index];
        }
        write_index++;
    }
    while (write_index < preferences->count) {
        memset(&preferences->items[write_index], 0, sizeof(preferences->items[write_index]));
        write_index++;
    }
    preferences->count -= *removed_count;
    return CUP_OK;
}

CupError tool_preferences_resolve(const InstallPolicy *policy,
                                  const ToolPreferences *preferences,
                                  const char *host_platform,
                                  const char *target_platform,
                                  const char *component,
                                  char *tool,
                                  size_t tool_size,
                                  ToolPreferenceSource *source) {
    PackageScope scope;
    const InstallDefault *official;
    int index;

    if (policy == NULL || preferences == NULL || tool == NULL || tool_size == 0 || source == NULL ||
        package_scope_init(&scope, component, host_platform, target_platform) != CUP_OK) {
        return CUP_ERR_INVALID_INPUT;
    }

    index = preference_index(preferences, &scope);
    if (index >= 0) {
        *source = TOOL_PREFERENCE_USER;
        return text_copy(tool, tool_size, preferences->items[index].tool);
    }

    official = install_policy_find_default(policy, host_platform, target_platform, component);
    if (official == NULL) {
        *source = TOOL_PREFERENCE_NONE;
        tool[0] = '\0';
        return CUP_ERR_NOT_AVAILABLE;
    }
    *source = TOOL_PREFERENCE_OFFICIAL_DEFAULT;
    return text_copy(tool, tool_size, official->tool);
}
