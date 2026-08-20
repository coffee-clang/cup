/*
 * Parses and validates the immutable install.cfg document containing official scoped defaults,
 * profiles and toolchains.
 */

#include "install_policy.h"

#include "filesystem.h"
#include "layout.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "system.h"
#include "text.h"
#include "version.h"

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

static int named_list_index(const InstallNamedList *lists, size_t count, const char *name) {
    size_t i;

    if (lists == NULL || text_is_empty(name)) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (strcmp(lists[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const InstallNamedList *install_policy_find_profile(const InstallPolicy *policy, const char *name) {
    if (policy != NULL) {
        int index = named_list_index(policy->profiles, policy->profile_count, name);

        return index < 0 ? NULL : &policy->profiles[index];
    }
    return NULL;
}

const InstallNamedList *install_policy_find_toolchain(const InstallPolicy *policy,
                                                      const char *name) {
    if (policy != NULL) {
        int index = named_list_index(policy->toolchains, policy->toolchain_count, name);

        return index < 0 ? NULL : &policy->toolchains[index];
    }
    return NULL;
}

/* Physical install.cfg parsing. Semantic lines accept trimmed whitespace; the schema marker must be
 * the first semantic record and duplicate or partially valid records are rejected. */
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
        registry_validate_tool(component, value) != CUP_OK || default_index(policy, &scope) >= 0 ||
        policy->default_count >= MAX_INSTALL_DEFAULTS) {
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
    if (named_list_index(lists, *count, name) >= 0 || *count >= capacity) {
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

CupError install_policy_load_path(InstallPolicy *policy, const char *path) {
    PersistentFileSnapshot snapshot;
    TextDocumentReader reader;
    CupError err;
    char line[MAX_INSTALL_POLICY_LINE_LEN];
    int has_line;
    int format_seen = 0;
    int missing;

    if (policy == NULL || text_is_empty(path)) {
        return CUP_ERR_INVALID_INPUT;
    }
    install_policy_init(policy);
    filesystem_snapshot_init(&snapshot);
    err = filesystem_snapshot_read(
        path, MAX_PERSISTENT_METADATA_BYTES, &snapshot, &missing);
    if (err != CUP_OK || missing) {
        return err != CUP_OK ? err : CUP_ERR_FILESYSTEM;
    }
    err = text_document_reader_init(&reader, snapshot.data, snapshot.size);
    if (err != CUP_OK) {
        filesystem_snapshot_release(&snapshot);
        return CUP_ERR_VALIDATION;
    }

    while (1) {
        char key[MAX_METADATA_KEY_LEN];
        char value[MAX_INSTALL_POLICY_LINE_LEN];

        err = text_document_read_line(&reader, line, sizeof(line), &has_line);
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

        if (!format_seen && strcmp(key, "format") != 0) {
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

    filesystem_snapshot_release(&snapshot);
    if (!format_seen || validate_policy(policy) != CUP_OK) {
        install_policy_init(policy);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;

invalid:
    fprintf(stderr, "Error: invalid installation policy line %zu.\n", reader.line_number);
    filesystem_snapshot_release(&snapshot);
    install_policy_init(policy);
    return err == CUP_ERR_FILESYSTEM || err == CUP_ERR_TEMPORARY ? err : CUP_ERR_VALIDATION;
}

/* Policy sources. Installed assets are authoritative for official cup builds, while development
 * builds may use the repository copy. */
CupError install_policy_load_development(InstallPolicy *policy) {
    return install_policy_load_path(policy, CUP_DEVELOPMENT_INSTALL_POLICY_PATH);
}

CupError install_policy_load(InstallPolicy *policy) {
    char path[MAX_PATH_LEN];
    CupError err;
    int exists;

    if (policy == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    install_policy_init(policy);
    err = layout_get_install_policy_path(path, sizeof(path));
    if (err != CUP_OK) {
        return err;
    }
    err = system_path_exists(path, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return install_policy_load_path(policy, path);
    }
#if !CUP_VERSION_OFFICIAL
    err = system_path_exists(CUP_DEVELOPMENT_INSTALL_POLICY_PATH, &exists);
    if (err != CUP_OK) {
        return err;
    }
    if (exists) {
        return install_policy_load_development(policy);
    }
#endif
    fprintf(stderr,
            "Error: installation policy not found. "
            "Run 'cup repair' to restore official configuration assets.\n");
    return CUP_ERR_VALIDATION;
}
