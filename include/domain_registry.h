#ifndef CUP_DOMAIN_REGISTRY_H
#define CUP_DOMAIN_REGISTRY_H

/*
 * Single compile-time registry source for the finite component/tool/platform domains.
 * Consumers expand these lists to build tables, counts, and bounded scope capacities.
 */

#define CUP_COMPONENT_REGISTRY(X) \
    X("compiler")                 \
    X("debugger")                 \
    X("linker")                   \
    X("formatter")                \
    X("linter")                   \
    X("language-server")          \
    X("analyzer")

#define CUP_TOOL_REGISTRY(X)       \
    X("compiler", "gcc")           \
    X("compiler", "clang")         \
    X("debugger", "gdb")           \
    X("debugger", "lldb")          \
    X("linker", "lld")             \
    X("linker", "ld")              \
    X("formatter", "clang-format") \
    X("linter", "clang-tidy")      \
    X("language-server", "clangd") \
    X("analyzer", "valgrind")

#define CUP_PLATFORM_REGISTRY(X) \
    X("linux", "x64")            \
    X("linux", "arm64")          \
    X("windows", "x64")          \
    X("macos", "x64")            \
    X("macos", "arm64")

#define CUP_COUNT_ENTRY(...) +1
enum {
    CUP_COMPONENT_COUNT = 0 CUP_COMPONENT_REGISTRY(CUP_COUNT_ENTRY),
    CUP_TOOL_COUNT = 0 CUP_TOOL_REGISTRY(CUP_COUNT_ENTRY),
    CUP_PLATFORM_COUNT = 0 CUP_PLATFORM_REGISTRY(CUP_COUNT_ENTRY),
    CUP_GLOBAL_SCOPE_COUNT = CUP_COMPONENT_COUNT * CUP_PLATFORM_COUNT * CUP_PLATFORM_COUNT
};
#undef CUP_COUNT_ENTRY

#endif /* CUP_DOMAIN_REGISTRY_H */
