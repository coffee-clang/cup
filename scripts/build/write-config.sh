#!/bin/sh

# Writes the complete build identity atomically and preserves its
# timestamp when compiler, flags, dependencies and official status are unchanged.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$SCRIPT_DIR/../lib/path-safety.sh"

output=${1:?output path is required}
: "${CUP_BUILD_PLATFORM:?CUP_BUILD_PLATFORM is required}"
: "${CUP_BUILD_CONFIGURATION:?CUP_BUILD_CONFIGURATION is required}"
: "${CUP_BUILD_CC:?CUP_BUILD_CC is required}"
: "${CUP_BUILD_DEPS_PREFIX:?CUP_BUILD_DEPS_PREFIX is required}"
: "${CUP_BUILD_OFFICIAL:?CUP_BUILD_OFFICIAL is required}"

fail() {
    printf 'build config: %s\n' "$*" >&2
    exit 1
}

single_line() {
    label=$1
    value=$2
    lf='
'
    cr=$(printf '\r')
    case "$value" in
        *"$lf"*|*"$cr"*) fail "$label must be a single-line value" ;;
    esac
}

reject_nul_or_cr() {
    file=$1
    label=$2
    if od -An -v -t x1 "$file" | awk '{ for (i = 1; i <= NF; ++i) if ($i == "00" || $i == "0d") exit 1 }'; then
        return 0
    fi
    fail "$label contains a NUL or carriage-return byte"
}

read_exact_field() {
    file=$1
    key=$2
    value=$(awk -F= -v key="$key" '
        $1 == key { count++; value = substr($0, length(key) + 2) }
        END {
            if (count != 1 || value == "") exit 1
            print value
        }
    ' "$file") || fail "$file must contain exactly one non-empty $key field"
    printf '%s\n' "$value"
}

first_line() {
    sed -n '1p' | tr -d '\r'
}

single_line platform "$CUP_BUILD_PLATFORM"
single_line configuration "$CUP_BUILD_CONFIGURATION"
single_line compiler_command "$CUP_BUILD_CC"
single_line deps_prefix "$CUP_BUILD_DEPS_PREFIX"
single_line official_build "$CUP_BUILD_OFFICIAL"
single_line windres_command "${CUP_BUILD_WINDRES:-}"

normalize_compiler_target() {
    target=$1
    case "$CUP_BUILD_PLATFORM:$target" in
        linux-x64:x86_64-*linux*|linux-x64:amd64-*linux*) printf '%s\n' linux-x64 ;;
        linux-arm64:aarch64-*linux*|linux-arm64:arm64-*linux*) printf '%s\n' linux-arm64 ;;
        macos-x64:x86_64-apple-darwin*) printf '%s\n' macos-x64 ;;
        macos-arm64:arm64-apple-darwin*|macos-arm64:aarch64-apple-darwin*) printf '%s\n' macos-arm64 ;;
        windows-x64:x86_64-w64-mingw32|windows-x64:x86_64-*-windows-gnu*) printf '%s\n' windows-x64 ;;
        *) return 1 ;;
    esac
}

extract_numeric_version() {
    printf '%s\n' "$1" | sed -n 's/.*[^0-9]\([0-9][0-9]*\(\.[0-9][0-9]*\)\{1,3\}\).*/\1/p' | sed -n '1p'
}

set -- $CUP_BUILD_CC
compiler_program=${1:-}
[ -n "$compiler_program" ] || fail 'compiler command is empty'
compiler_path=$(command -v "$compiler_program" 2>/dev/null || printf missing)
compiler_target=$($CUP_BUILD_CC -dumpmachine 2>/dev/null | first_line)
compiler_version=$($CUP_BUILD_CC --version 2>/dev/null | first_line)
compiler_target_normalized=$(normalize_compiler_target "$compiler_target" || true)
compiler_numeric=$($CUP_BUILD_CC -dumpfullversion -dumpversion 2>/dev/null | first_line || true)
[ -n "$compiler_numeric" ] || compiler_numeric=$($CUP_BUILD_CC -dumpversion 2>/dev/null | first_line || true)
[ "$compiler_path" != missing ] || fail "compiler command was not found: $compiler_program"
[ -n "$compiler_target" ] || fail 'compiler target is empty'
[ -n "$compiler_target_normalized" ] || fail "compiler target '$compiler_target' does not match $CUP_BUILD_PLATFORM"
[ -n "$compiler_version" ] || fail 'compiler version is empty'
[ -n "$compiler_numeric" ] || fail 'compiler numeric version is empty'

windres_command=${CUP_BUILD_WINDRES:-}
windres_path=
windres_version=
windres_numeric=
windres_target_normalized=
if [ -n "$windres_command" ]; then
    set -- $windres_command
    windres_program=${1:-}
    windres_path=$(command -v "$windres_program" 2>/dev/null || printf missing)
    windres_version=$($windres_command --version 2>/dev/null | first_line || true)
    windres_numeric=$(extract_numeric_version "$windres_version")
    [ "$windres_path" != missing ] || fail "windres command was not found: $windres_program"
    [ -n "$windres_version" ] || fail 'windres version is empty'
    [ -n "$windres_numeric" ] || fail 'windres numeric version is empty'
    [ "$CUP_BUILD_PLATFORM" = windows-x64 ] || fail 'windres is only valid for windows-x64 builds'
    windres_target_normalized=windows-x64
fi

cup_path_check_directory_chain "$CUP_BUILD_DEPS_PREFIX" 0 "dependency prefix" ||
    fail "dependency prefix has an unsafe path"
dependency_file=$CUP_BUILD_DEPS_PREFIX/.cup-dependencies
cup_path_require_regular_file "$dependency_file" "dependency metadata" ||
    fail "dependency metadata is missing or unsafe: $dependency_file"
reject_nul_or_cr "$dependency_file" 'dependency metadata'
dependency_prefix_format=$(read_exact_field "$dependency_file" prefix_format)
dependency_platform=$(read_exact_field "$dependency_file" platform)
dependency_profile=$(read_exact_field "$dependency_file" profile)
dependency_build_revision=$(read_exact_field "$dependency_file" build_revision)
dependency_source_lock_sha256=$(read_exact_field "$dependency_file" source_lock_sha256)
dependency_toolchain_sha256=$(read_exact_field "$dependency_file" toolchain_sha256)

host_system=$(uname -s 2>/dev/null || printf unknown)
host_machine=$(uname -m 2>/dev/null || printf unknown)

for pair in \
    "platform:$CUP_BUILD_PLATFORM" \
    "configuration:$CUP_BUILD_CONFIGURATION" \
    "host_system:$host_system" \
    "host_machine:$host_machine" \
    "compiler_command:$CUP_BUILD_CC" \
    "compiler_path:$compiler_path" \
    "compiler_target:$compiler_target" \
    "compiler_target_normalized:$compiler_target_normalized" \
    "compiler_version:$compiler_version" \
    "compiler_numeric:$compiler_numeric" \
    "windres_command:$windres_command" \
    "windres_path:$windres_path" \
    "windres_version:$windres_version" \
    "windres_numeric:$windres_numeric" \
    "windres_target_normalized:$windres_target_normalized" \
    "cppflags:${CUP_BUILD_CPPFLAGS:-}" \
    "cflags:${CUP_BUILD_CFLAGS:-}" \
    "ldflags:${CUP_BUILD_LDFLAGS:-}" \
    "ldlibs:${CUP_BUILD_LDLIBS:-}" \
    "deps_prefix:$CUP_BUILD_DEPS_PREFIX" \
    "dependency_prefix_format:$dependency_prefix_format" \
    "dependency_platform:$dependency_platform" \
    "dependency_profile:$dependency_profile" \
    "dependency_build_revision:$dependency_build_revision" \
    "dependency_source_lock_sha256:$dependency_source_lock_sha256" \
    "dependency_toolchain_sha256:$dependency_toolchain_sha256" \
    "official_build:$CUP_BUILD_OFFICIAL"; do
    single_line "${pair%%:*}" "${pair#*:}"
done

if [ -n "${CUP_BUILD_ROOT:-}" ]; then
    cup_path_require_build_root "$CUP_BUILD_ROOT" || fail "invalid build root: $CUP_BUILD_ROOT"
    cup_path_prepare_child_file "$CUP_BUILD_ROOT" "$output" "build configuration" || exit 1
else
    cup_path_prepare_file_target "$output" "build configuration" || exit 1
fi
{
    printf 'format=3\n'
    printf 'platform=%s\n' "$CUP_BUILD_PLATFORM"
    printf 'configuration=%s\n' "$CUP_BUILD_CONFIGURATION"
    printf 'host_system=%s\n' "$host_system"
    printf 'host_machine=%s\n' "$host_machine"
    printf 'compiler_command=%s\n' "$CUP_BUILD_CC"
    printf 'compiler_path=%s\n' "$compiler_path"
    printf 'compiler_target=%s\n' "$compiler_target"
    printf 'compiler_target_normalized=%s\n' "$compiler_target_normalized"
    printf 'compiler_version=%s\n' "$compiler_version"
    printf 'compiler_numeric=%s\n' "$compiler_numeric"
    printf 'windres_command=%s\n' "$windres_command"
    printf 'windres_path=%s\n' "$windres_path"
    printf 'windres_version=%s\n' "$windres_version"
    printf 'windres_numeric=%s\n' "$windres_numeric"
    printf 'windres_target_normalized=%s\n' "$windres_target_normalized"
    printf 'cppflags=%s\n' "${CUP_BUILD_CPPFLAGS:-}"
    printf 'cflags=%s\n' "${CUP_BUILD_CFLAGS:-}"
    printf 'ldflags=%s\n' "${CUP_BUILD_LDFLAGS:-}"
    printf 'ldlibs=%s\n' "${CUP_BUILD_LDLIBS:-}"
    printf 'deps_prefix=%s\n' "$CUP_BUILD_DEPS_PREFIX"
    printf 'dependency_prefix_format=%s\n' "$dependency_prefix_format"
    printf 'dependency_platform=%s\n' "$dependency_platform"
    printf 'dependency_profile=%s\n' "$dependency_profile"
    printf 'dependency_build_revision=%s\n' "$dependency_build_revision"
    printf 'dependency_source_lock_sha256=%s\n' "$dependency_source_lock_sha256"
    printf 'dependency_toolchain_sha256=%s\n' "$dependency_toolchain_sha256"
    printf 'official_build=%s\n' "$CUP_BUILD_OFFICIAL"
} | cup_path_write_file "$output" 0644 if-different || fail "could not write $output"
