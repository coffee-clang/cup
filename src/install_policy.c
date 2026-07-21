/*
 * Parses and validates the immutable install.cfg document containing official scoped defaults,
 * profiles and toolchains.
 */

#include "install_policy.h"

#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define INSTALL_POLICY_FORMAT "1"

static int name_is_canonical(const char *name) {
    const unsigned char *cursor;

    if (!path_is_safe_identifier(name)) {
        return 0;
    }
    for (cursor = (const unsigned char *)name; *cursor != '\0'; ++cursor) {
        if (*cursor >= 'A' && *cursor <= 'Z') {
            return 0;
        }
    }
    return 1;
}

/* Scoped lookup helpers. Policy entries are keyed by component, host and target; no global fallback
 * is inferred here. */
static int default_index(const InstallPolicy *policy, const PackageScope *scope) {
    size_t i;

    for (i = 0; i < policy->default_count; ++i) {
        if (package_scope_equals(&policy->defaults[i].scope, scope)) {
            return (int)i;
        }
    }
    return -1;
}

const InstallDefault *install_policy_find_default(const InstallPolicy *policy,
                                                  const char *host_platform,
                                                  const char *target_platform,
                                                  const char *component) {
    PackageScope scope;
    int index;

    if (policy == NULL ||
        package_scope_init(&scope, component, host_platform, target_platform) != CUP_OK) {
        return NULL;
    }
    index = default_index(policy, &scope);
    return index < 0 ? NULL : &policy->defaults[index];
}

static InstallNamedList *find_named_list_mutable(InstallNamedList *lists,
                                                 size_t count,
                                                 const char *name) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (strcmp(lists[i].name, name) == 0) {
            return &lists[i];
        }
    }
    return NULL;
}

static const InstallNamedList *find_named_list(const InstallNamedList *lists,
                                               size_t count,
                                               const char *name) {
    size_t i;

    if (lists == NULL || text_is_empty(name)) {
        return NULL;
    }
    for (i = 0; i < count; ++i) {
        if (strcmp(lists[i].name, name) == 0) {
            return &lists[i];
        }
    }
    return NULL;
}

const InstallNamedList *install_policy_find_profile(const InstallPolicy *policy, const char *name) {
    return policy == NULL ? NULL : find_named_list(policy->profiles, policy->profile_count, name);
}

const InstallNamedList *install_policy_find_toolchain(const InstallPolicy *policy,
                                                      const char *name) {
    return policy == NULL ? NULL
                          : find_named_list(policy->toolchains, policy->toolchain_count, name);
}

/* Physical install.cfg parsing. The parser accepts one canonical spelling and rejects duplicate or
 * partially valid records. */
static CupError parse_list(char *value,
                           char items[][MAX_IDENTIFIER_LEN],
                           size_t capacity,
                           size_t *count) {
    char *cursor = value;

    *count = 0;
    while (cursor != NULL) {
        char *separator = strchr(cursor, ',');
        char *item;
        size_t i;

        if (separator != NULL) {
            *separator = '\0';
        }
        item = text_trim(cursor);
        if (text_is_empty(item) || !name_is_canonical(item) || *count >= capacity) {
            return CUP_ERR_INVALID_INPUT;
        }
        for (i = 0; i < *count; ++i) {
            if (strcmp(items[i], item) == 0) {
                return CUP_ERR_INVALID_INPUT;
            }
        }
        if (text_copy(items[*count], MAX_IDENTIFIER_LEN, item) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        (*count)++;
        cursor = separator == NULL ? NULL : separator + 1;
    }
    return *count == 0 ? CUP_ERR_INVALID_INPUT : CUP_OK;
}

static CupError parse_default(InstallPolicy *policy, char *key, const char *value) {
    char prefix[MAX_IDENTIFIER_LEN];
    char host[MAX_PLATFORM_LEN];
    char target[MAX_PLATFORM_LEN];
    char component[MAX_IDENTIFIER_LEN];
    TextBuffer parts[4];
    InstallDefault *entry;
    PackageScope scope;
    CupError err;

    parts[0] = (TextBuffer){prefix, sizeof(prefix)};
    parts[1] = (TextBuffer){host, sizeof(host)};
    parts[2] = (TextBuffer){target, sizeof(target)};
    parts[3] = (TextBuffer){component, sizeof(component)};
    if (text_split_exact(key, '.', parts, 4) != CUP_OK || strcmp(prefix, "default") != 0 ||
        package_scope_init(&scope, component, host, target) != CUP_OK ||
        registry_validate_tool(component, value) != CUP_OK || !name_is_canonical(value) ||
        default_index(policy, &scope) >= 0 || policy->default_count >= MAX_INSTALL_DEFAULTS) {
        return CUP_ERR_INVALID_INPUT;
    }

    entry = &policy->defaults[policy->default_count++];
    memset(entry, 0, sizeof(*entry));
    entry->scope = scope;
    err = text_copy(entry->tool, sizeof(entry->tool), value);
    return err;
}

static CupError validate_profile_items(const InstallNamedList *list) {
    size_t i;

    for (i = 0; i < list->item_count; ++i) {
        if (registry_validate_component(list->items[i]) != CUP_OK) {
            return CUP_ERR_INVALID_INPUT;
        }
    }
    return CUP_OK;
}

static CupError validate_toolchain_items(const InstallNamedList *list) {
    char components[MAX_INSTALL_LIST_ITEMS][MAX_IDENTIFIER_LEN];
    size_t component_count = 0;
    size_t i;

    for (i = 0; i < list->item_count; ++i) {
        char component[MAX_IDENTIFIER_LEN];
        size_t previous;

        if (registry_find_tool_component(list->items[i], component, sizeof(component)) != CUP_OK) {
            return CUP_ERR_INVALID_INPUT;
        }
        for (previous = 0; previous < component_count; ++previous) {
            if (strcmp(components[previous], component) == 0) {
                return CUP_ERR_INVALID_INPUT;
            }
        }
        if (text_copy(components[component_count], MAX_IDENTIFIER_LEN, component) != CUP_OK) {
            return CUP_ERR_BUFFER_TOO_SMALL;
        }
        component_count++;
    }
    return CUP_OK;
}

static CupError parse_named_list(InstallPolicy *policy,
                                 char *key,
                                 char *value,
                                 const char *expected_prefix) {
    char prefix[MAX_IDENTIFIER_LEN];
    char name[MAX_IDENTIFIER_LEN];
    TextBuffer parts[2];
    InstallNamedList *lists;
    size_t *count;
    size_t capacity;
    InstallNamedList *list;
    CupError err;

    parts[0] = (TextBuffer){prefix, sizeof(prefix)};
    parts[1] = (TextBuffer){name, sizeof(name)};
    if (text_split_exact(key, '.', parts, 2) != CUP_OK || strcmp(prefix, expected_prefix) != 0 ||
        !name_is_canonical(name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(expected_prefix, "profile") == 0) {
        lists = policy->profiles;
        count = &policy->profile_count;
        capacity = MAX_INSTALL_PROFILES;
    } else {
        lists = policy->toolchains;
        count = &policy->toolchain_count;
        capacity = MAX_INSTALL_TOOLCHAINS;
    }
    if (find_named_list_mutable(lists, *count, name) != NULL || *count >= capacity) {
        return CUP_ERR_INVALID_INPUT;
    }

    list = &lists[(*count)++];
    memset(list, 0, sizeof(*list));
    err = text_copy(list->name, sizeof(list->name), name);
    if (err == CUP_OK) {
        err = parse_list(value, list->items, MAX_INSTALL_LIST_ITEMS, &list->item_count);
    }
    if (err != CUP_OK) {
        return err;
    }
    return strcmp(expected_prefix, "profile") == 0 ? validate_profile_items(list)
                                                   : validate_toolchain_items(list);
}

/* Cross-record validation. Profiles and toolchains are checked only after the complete file has
 * been parsed. */
static CupError validate_policy(const InstallPolicy *policy) {
    return policy->default_count == 0 || policy->profile_count == 0 || policy->toolchain_count == 0
               ? CUP_ERR_INVALID_INPUT
               : CUP_OK;
}

void install_policy_init(InstallPolicy *policy) {
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
}

CupError install_policy_load_path(InstallPolicy *policy,
                                  const char *path,
                                  InstallPolicySource source) {
    FILE *file;
    CupError err;
    char line[MAX_INSTALL_POLICY_LINE_LEN];
    size_t line_number = 0;
    int has_line;
    int format_seen = 0;

    if (policy == NULL || text_is_empty(path) || source == INSTALL_POLICY_SOURCE_NONE) {
        return CUP_ERR_INVALID_INPUT;
    }
    install_policy_init(policy);
    file = fopen(path, "rb");
    if (file == NULL) {
        return CUP_ERR_FILESYSTEM;
    }

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
            if (format_seen || strcmp(value, INSTALL_POLICY_FORMAT) != 0) {
                err = CUP_ERR_VALIDATION;
                goto invalid;
            }
            format_seen = 1;
        } else if (strncmp(key, "default.", 8) == 0) {
            err = parse_default(policy, key, value);
            if (err != CUP_OK) {
                goto invalid;
            }
        } else if (strncmp(key, "profile.", 8) == 0) {
            err = parse_named_list(policy, key, value, "profile");
            if (err != CUP_OK) {
                goto invalid;
            }
        } else if (strncmp(key, "toolchain.", 10) == 0) {
            err = parse_named_list(policy, key, value, "toolchain");
            if (err != CUP_OK) {
                goto invalid;
            }
        } else {
            err = CUP_ERR_VALIDATION;
            goto invalid;
        }
    }

    if (fclose(file) != 0) {
        install_policy_init(policy);
        return CUP_ERR_FILESYSTEM;
    }
    if (!format_seen || validate_policy(policy) != CUP_OK ||
        text_copy(policy->path, sizeof(policy->path), path) != CUP_OK) {
        install_policy_init(policy);
        return CUP_ERR_VALIDATION;
    }
    policy->source = source;
    return CUP_OK;

invalid:
    fprintf(stderr, "Error: invalid installation policy line %zu.\n", line_number);
    fclose(file);
    install_policy_init(policy);
    return err == CUP_ERR_FILESYSTEM || err == CUP_ERR_TEMPORARY ? err : CUP_ERR_VALIDATION;
}

/* Policy sources. Installed assets are authoritative for official CUP builds, while development
 * builds may use the repository copy. */
CupError install_policy_load_installed(InstallPolicy *policy) {
    char path[MAX_PATH_LEN];
    CupError err = layout_get_install_policy_path(path, sizeof(path));

    return err == CUP_OK ? install_policy_load_path(policy, path, INSTALL_POLICY_SOURCE_INSTALLED)
                         : err;
}

CupError install_policy_load_development(InstallPolicy *policy) {
    return install_policy_load_path(
        policy, CUP_DEVELOPMENT_INSTALL_POLICY_PATH, INSTALL_POLICY_SOURCE_DEVELOPMENT);
}

CupError install_policy_load(InstallPolicy *policy) {
    char path[MAX_PATH_LEN];
    CupError err;
    int exists;

    if (policy == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    err = layout_get_install_policy_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return install_policy_load_installed(policy);
    }
    err = system_path_exists(CUP_DEVELOPMENT_INSTALL_POLICY_PATH, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return install_policy_load_development(policy);
    }
    fprintf(stderr,
            "Error: installation policy not found. "
            "Run 'cup repair' to restore official configuration assets.\n");
    return CUP_ERR_VALIDATION;
}
