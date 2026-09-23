/* Official scoped defaults, profiles and curated toolchains compiled into this cup generation. */

#include "install_policy.h"

#include "registry.h"
#include "text.h"

#include <string.h>

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

void install_policy_init(InstallPolicy *policy) {
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
    }
}

typedef struct {
    const char *host;
    const char *target;
    const char *component;
    const char *tool;
} CompiledDefault;

typedef struct {
    const char *name;
    const char *const *items;
    size_t count;
} CompiledList;

static const CompiledDefault COMPILED_DEFAULTS[] = {
    {"linux-arm64", "linux-arm64", "compiler", "clang"},
    {"linux-arm64", "linux-arm64", "debugger", "lldb"},
    {"linux-arm64", "linux-arm64", "linker", "lld"},
    {"linux-arm64", "linux-arm64", "formatter", "clang-format"},
    {"linux-arm64", "linux-arm64", "linter", "clang-tidy"},
    {"linux-arm64", "linux-arm64", "language-server", "clangd"},
    {"linux-arm64", "linux-arm64", "analyzer", "valgrind"},
    {"linux-x64", "linux-x64", "compiler", "clang"},
    {"linux-x64", "linux-x64", "debugger", "lldb"},
    {"linux-x64", "linux-x64", "linker", "lld"},
    {"linux-x64", "linux-x64", "formatter", "clang-format"},
    {"linux-x64", "linux-x64", "linter", "clang-tidy"},
    {"linux-x64", "linux-x64", "language-server", "clangd"},
    {"linux-x64", "linux-x64", "analyzer", "valgrind"},
    {"linux-x64", "windows-x64", "compiler", "gcc"},
    {"linux-x64", "windows-x64", "linker", "ld"},
    {"macos-arm64", "macos-arm64", "compiler", "clang"},
    {"macos-arm64", "macos-arm64", "debugger", "lldb"},
    {"macos-arm64", "macos-arm64", "linker", "lld"},
    {"macos-arm64", "macos-arm64", "formatter", "clang-format"},
    {"macos-arm64", "macos-arm64", "linter", "clang-tidy"},
    {"macos-arm64", "macos-arm64", "language-server", "clangd"},
    {"macos-x64", "macos-x64", "compiler", "clang"},
    {"macos-x64", "macos-x64", "debugger", "lldb"},
    {"macos-x64", "macos-x64", "linker", "lld"},
    {"macos-x64", "macos-x64", "formatter", "clang-format"},
    {"macos-x64", "macos-x64", "linter", "clang-tidy"},
    {"macos-x64", "macos-x64", "language-server", "clangd"},
    {"windows-x64", "windows-x64", "compiler", "clang"},
    {"windows-x64", "windows-x64", "debugger", "lldb"},
    {"windows-x64", "windows-x64", "linker", "lld"},
    {"windows-x64", "windows-x64", "formatter", "clang-format"},
    {"windows-x64", "windows-x64", "linter", "clang-tidy"},
    {"windows-x64", "windows-x64", "language-server", "clangd"}
};

static const char *const PROFILE_MINIMAL[] = {"compiler", "linker"};
static const char *const PROFILE_STANDARD[] = {"compiler", "linker", "debugger", "language-server"};
static const char *const PROFILE_EXTENDED[] = {
    "compiler", "linker", "debugger", "language-server", "formatter", "linter"
};
static const char *const TOOLCHAIN_LLVM[] = {
    "clang", "lldb", "lld", "clang-format", "clang-tidy", "clangd"
};
static const char *const TOOLCHAIN_GNU[] = {"gcc", "gdb", "ld"};

static const CompiledList COMPILED_PROFILES[] = {
    {"minimal", PROFILE_MINIMAL, sizeof(PROFILE_MINIMAL) / sizeof(PROFILE_MINIMAL[0])},
    {"standard", PROFILE_STANDARD, sizeof(PROFILE_STANDARD) / sizeof(PROFILE_STANDARD[0])},
    {"extended", PROFILE_EXTENDED, sizeof(PROFILE_EXTENDED) / sizeof(PROFILE_EXTENDED[0])}
};
static const CompiledList COMPILED_TOOLCHAINS[] = {
    {"llvm", TOOLCHAIN_LLVM, sizeof(TOOLCHAIN_LLVM) / sizeof(TOOLCHAIN_LLVM[0])},
    {"gnu", TOOLCHAIN_GNU, sizeof(TOOLCHAIN_GNU) / sizeof(TOOLCHAIN_GNU[0])}
};

static CupError copy_compiled_list(InstallNamedList *destination, const CompiledList *source) {
    size_t i;

    if (destination == NULL || source == NULL || source->count > MAX_INSTALL_LIST_ITEMS ||
        text_copy(destination->name, sizeof(destination->name), source->name) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    destination->item_count = source->count;
    for (i = 0; i < source->count; ++i) {
        if (text_copy(destination->items[i], sizeof(destination->items[i]), source->items[i]) != CUP_OK) {
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }
    return CUP_OK;
}

CupError install_policy_load(InstallPolicy *policy) {
    size_t i;

    if (policy == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    install_policy_init(policy);
    if (sizeof(COMPILED_DEFAULTS) / sizeof(COMPILED_DEFAULTS[0]) > MAX_INSTALL_DEFAULTS ||
        sizeof(COMPILED_PROFILES) / sizeof(COMPILED_PROFILES[0]) > MAX_INSTALL_PROFILES ||
        sizeof(COMPILED_TOOLCHAINS) / sizeof(COMPILED_TOOLCHAINS[0]) > MAX_INSTALL_TOOLCHAINS) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    for (i = 0; i < sizeof(COMPILED_DEFAULTS) / sizeof(COMPILED_DEFAULTS[0]); ++i) {
        InstallDefault *entry = &policy->defaults[policy->default_count++];
        const CompiledDefault *source = &COMPILED_DEFAULTS[i];

        if (package_scope_init(&entry->scope, source->component, source->host, source->target) != CUP_OK ||
            registry_validate_tool(source->component, source->tool) != CUP_OK ||
            text_copy(entry->tool, sizeof(entry->tool), source->tool) != CUP_OK) {
            install_policy_init(policy);
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }
    for (i = 0; i < sizeof(COMPILED_PROFILES) / sizeof(COMPILED_PROFILES[0]); ++i) {
        if (copy_compiled_list(&policy->profiles[policy->profile_count++], &COMPILED_PROFILES[i]) != CUP_OK) {
            install_policy_init(policy);
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }
    for (i = 0; i < sizeof(COMPILED_TOOLCHAINS) / sizeof(COMPILED_TOOLCHAINS[0]); ++i) {
        if (copy_compiled_list(&policy->toolchains[policy->toolchain_count++], &COMPILED_TOOLCHAINS[i]) != CUP_OK) {
            install_policy_init(policy);
            return CUP_ERR_INCONSISTENT_STATE;
        }
    }
    return CUP_OK;
}
