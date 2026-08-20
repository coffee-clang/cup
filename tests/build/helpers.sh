#!/usr/bin/env bash

# Compiles test-only helper programs under Makefile ownership.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

list_registered_helpers() {
    local platform=$1
    local name
    awk '/^[[:space:]]*compile_helper[[:space:]]+[A-Za-z0-9_-]+/ { print $2 }' "$0" |
        while IFS= read -r name; do
            case "$platform:$name" in
                windows-x64:archive-fixture) continue ;;
                windows-x64:*) printf '%s.exe\n' "$name" ;;
                *) printf '%s\n' "$name" ;;
            esac
        done
}

if [ "${1:-}" = --list ]; then
    list_platform=${2:-${CUP_TEST_PLATFORM:-}}
    case "$list_platform" in
        linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
        *)
            printf 'Usage: %s --list <platform>\n' "$0" >&2
            exit 2
            ;;
    esac
    list_registered_helpers "$list_platform"
    exit 0
fi

. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
cup_test_require_dependencies
PLATFORM="$CUP_TEST_PLATFORM"
CONFIGURATION="${CUP_TEST_CONFIGURATION:-development}"
CC="${CC:-}"
[ -n "$CC" ] || {
    printf 'CC must be provided by the Makefile.\n' >&2
    exit 2
}
case "$CONFIGURATION" in
    development|debug|coverage|sanitizers|release) ;;
    *)
        printf 'Unsupported helper configuration: %s\n' "$CONFIGURATION" >&2
        exit 2
        ;;
esac
: "${CUP_TEST_CPPFLAGS?CUP_TEST_CPPFLAGS must be provided by the Makefile}"
: "${CUP_TEST_CFLAGS?CUP_TEST_CFLAGS must be provided by the Makefile}"
: "${CUP_TEST_LDFLAGS?CUP_TEST_LDFLAGS must be provided by the Makefile}"
read -a TEST_CPPFLAGS <<<"$CUP_TEST_CPPFLAGS"
read -a TEST_CFLAGS <<<"$CUP_TEST_CFLAGS"
read -a TEST_LDFLAGS <<<"$CUP_TEST_LDFLAGS"
case "$PLATFORM" in
    windows-x64)
        EXE_SUFFIX=.exe
        PLATFORM_LIBS='-lws2_32 -liphlpapi'
        ;;
    *)
        EXE_SUFFIX=
        PLATFORM_LIBS=
        ;;
esac
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
OUT_FINAL="$TEST_BUILD_ROOT/$PLATFORM/$CONFIGURATION/tests/helpers"
OUT_PARENT=${OUT_FINAL%/helpers}
pkg_path="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig"
COVERAGE_ENTRY_SOURCE=
case "$PLATFORM:$CONFIGURATION" in
    macos-*:coverage)
        COVERAGE_ENTRY_SOURCE="$ROOT/tests/helpers/coverage-entry.c"
        ;;
esac
cup_test_load_path_safety || exit 1
cup_test_build_root_owned || exit 1
cup_path_prepare_child_directory "$TEST_BUILD_ROOT" "$OUT_PARENT" \
    'test-helper output parent' || exit 1
OUT=$(cup_path_create_unique_directory \
    "$OUT_PARENT/.helpers.XXXXXX" 'test-helper staging' 0755) || exit 1
GCOV_OUTPUT_DIR=
case "$PLATFORM:$CONFIGURATION" in
    linux-*:coverage|windows-x64:coverage)
        GCOV_OUTPUT_DIR=$(realpath --relative-to="$ROOT" "$OUT") || exit 1
        ;;
esac
cleanup_helper_staging() {
    if [ -n "${OUT:-}" ] && { [ -e "$OUT" ] || [ -L "$OUT" ]; }; then
        cup_path_remove_child_tree "$TEST_BUILD_ROOT" "$OUT" \
            'test-helper staging' >/dev/null 2>&1 || true
    fi
}
trap cleanup_helper_staging EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

compile_helper() {
    name=$1
    source=$2
    shift 2
    case "$source" in
        "$ROOT"/*) source=${source#"$ROOT"/} ;;
    esac
    output="$OUT/$name$EXE_SUFFIX"
    output_arg=$output
    [ -z "$GCOV_OUTPUT_DIR" ] || output_arg="$GCOV_OUTPUT_DIR/$name$EXE_SUFFIX"
    printf '==> Compiling test helper: %s\n' "$name"
    compile_command=("$CC" "${TEST_CPPFLAGS[@]}" "${TEST_CFLAGS[@]}")
    if [ -n "$GCOV_OUTPUT_DIR" ]; then
        compile_command+=(
            "-fprofile-dir=$OUT_FINAL"
            "-fprofile-prefix-path=$ROOT/$GCOV_OUTPUT_DIR"
        )
    fi
    if [ -n "$COVERAGE_ENTRY_SOURCE" ]; then
        entry_name=${name//-/_}
        entry_name="cup_coverage_${entry_name}_main"
        coverage_entry_source=${COVERAGE_ENTRY_SOURCE#"$ROOT"/}
        (cd "$ROOT" && "${compile_command[@]}" \
            "-Dmain=$entry_name" \
            "-DCUP_COVERAGE_ENTRY=$entry_name" \
            -I"$DEPS_PREFIX/include" \
            "$source" "$coverage_entry_source" "${TEST_LDFLAGS[@]}" "$@" \
            -o "$output_arg")
    else
        (cd "$ROOT" && "${compile_command[@]}" \
            -I"$DEPS_PREFIX/include" "$source" "${TEST_LDFLAGS[@]}" "$@" \
            -o "$output_arg")
    fi
}

if [ "$PLATFORM" != windows-x64 ]; then
    archive_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
        PKG_CONFIG_SYSROOT_DIR= pkg-config --static --libs libarchive)
    compile_helper archive-fixture "$ROOT/tests/helpers/archive-fixture.c" \
        $archive_libs
    compile_helper process-group "$ROOT/tests/helpers/process-group.c"
fi

event_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
    PKG_CONFIG_SYSROOT_DIR= \
    pkg-config --static --libs libevent_extra libevent_core)
compile_helper network-helper "$ROOT/tests/helpers/network-helper.c" \
    $event_libs $PLATFORM_LIBS
compile_helper binary-patch "$ROOT/tests/helpers/binary-patch.c"

expected_list="$OUT/.expected-helpers"
actual_list="$OUT/.actual-helpers"
list_registered_helpers "$PLATFORM" | LC_ALL=C sort > "$expected_list"
: > "$actual_list"
for helper_binary in "$OUT"/*; do
    [ -f "$helper_binary" ] || continue
    case "$helper_binary" in
        *.gcda|*.gcno|"$expected_list"|"$actual_list") continue ;;
    esac
    [ -x "$helper_binary" ] || continue
    basename "$helper_binary" >> "$actual_list"
done
LC_ALL=C sort -o "$actual_list" "$actual_list"
if [ "$(cat "$expected_list")" != "$(cat "$actual_list")" ]; then
    printf 'Expected test helpers:\n' >&2
    cat "$expected_list" >&2
    printf 'Compiled test helpers:\n' >&2
    cat "$actual_list" >&2
    exit 1
fi
rm -f -- "$expected_list" "$actual_list"

if [ -e "$OUT_FINAL" ] || [ -L "$OUT_FINAL" ]; then
    cup_path_check_directory_chain "$OUT_FINAL" 0 \
        'previous test-helper output' || exit 1
    cup_path_remove_child_tree "$TEST_BUILD_ROOT" "$OUT_FINAL" \
        'previous test-helper output' || exit 1
fi
cup_path_move_entry "$OUT" "$OUT_FINAL" ||
    { printf 'Could not publish complete test-helper output.\n' >&2; exit 1; }
OUT=
trap - EXIT HUP INT TERM
printf 'All test helpers compiled for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
