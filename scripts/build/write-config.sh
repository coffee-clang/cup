#!/bin/sh

# Purpose: Writes the complete build identity atomically and preserves its
# timestamp when compiler, flags, dependencies and official status are unchanged.
set -eu

output=${1:?output path is required}
: "${CUP_BUILD_PLATFORM:?CUP_BUILD_PLATFORM is required}"
: "${CUP_BUILD_CONFIGURATION:?CUP_BUILD_CONFIGURATION is required}"
: "${CUP_BUILD_CC:?CUP_BUILD_CC is required}"
: "${CUP_BUILD_DEPS_PREFIX:?CUP_BUILD_DEPS_PREFIX is required}"
: "${CUP_BUILD_OFFICIAL:?CUP_BUILD_OFFICIAL is required}"

first_line() {
    sed -n '1p' | tr -d '\r'
}

# Capture the exact compiler and optional resource compiler selected by Make.
set -- $CUP_BUILD_CC
compiler_program=${1:-}
compiler_path=$(command -v "$compiler_program" 2>/dev/null || printf missing)
compiler_target=$($CUP_BUILD_CC -dumpmachine 2>/dev/null | first_line)
compiler_version=$($CUP_BUILD_CC --version 2>/dev/null | first_line)
compiler_numeric=$($CUP_BUILD_CC -dumpfullversion -dumpversion 2>/dev/null | first_line || true)
[ -n "$compiler_numeric" ] ||
    compiler_numeric=$($CUP_BUILD_CC -dumpversion 2>/dev/null | first_line || true)

windres_command=${CUP_BUILD_WINDRES:-}
windres_path=
windres_version=
if [ -n "$windres_command" ]; then
    set -- $windres_command
    windres_program=${1:-}
    windres_path=$(command -v "$windres_program" 2>/dev/null || printf missing)
    windres_version=$($windres_command --version 2>/dev/null | first_line || true)
fi

# Bind object reuse to the verified dependency generation.
dependency_file=$CUP_BUILD_DEPS_PREFIX/.cup-deps-config
dependency_identity=$(sed -n 's/^dependency_id=//p' "$dependency_file" 2>/dev/null || true)
case "$dependency_identity" in
    *[!0-9a-f]*|'') dependency_identity=missing ;;
esac
[ "${#dependency_identity}" -eq 64 ] || dependency_identity=missing

# Replace the stamp only when its semantic contents change.
mkdir -p "$(dirname "$output")"
temporary=$output.tmp.$$
trap 'rm -f "$temporary"' EXIT HUP INT TERM

{
    printf 'format=1\n'
    printf 'platform=%s\n' "$CUP_BUILD_PLATFORM"
    printf 'configuration=%s\n' "$CUP_BUILD_CONFIGURATION"
    printf 'host_system=%s\n' "$(uname -s 2>/dev/null || printf unknown)"
    printf 'host_machine=%s\n' "$(uname -m 2>/dev/null || printf unknown)"
    printf 'compiler_command=%s\n' "$CUP_BUILD_CC"
    printf 'compiler_path=%s\n' "$compiler_path"
    printf 'compiler_target=%s\n' "$compiler_target"
    printf 'compiler_version=%s\n' "$compiler_version"
    printf 'compiler_numeric=%s\n' "$compiler_numeric"
    printf 'windres_command=%s\n' "$windres_command"
    printf 'windres_path=%s\n' "$windres_path"
    printf 'windres_version=%s\n' "$windres_version"
    printf 'cppflags=%s\n' "${CUP_BUILD_CPPFLAGS:-}"
    printf 'cflags=%s\n' "${CUP_BUILD_CFLAGS:-}"
    printf 'ldflags=%s\n' "${CUP_BUILD_LDFLAGS:-}"
    printf 'ldlibs=%s\n' "${CUP_BUILD_LDLIBS:-}"
    printf 'deps_prefix=%s\n' "$CUP_BUILD_DEPS_PREFIX"
    printf 'dependency_identity=%s\n' "$dependency_identity"
    printf 'official_build=%s\n' "$CUP_BUILD_OFFICIAL"
} >"$temporary"

if [ -f "$output" ] && cmp -s "$temporary" "$output"; then
    rm -f "$temporary"
else
    mv -f "$temporary" "$output"
fi
trap - EXIT HUP INT TERM
