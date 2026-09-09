#!/usr/bin/env bash

# Compiles test-only helper programs under Makefile ownership.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

helper_scope_applies() {
    local scope=$1
    local platform=$2

    case "$scope:$platform" in
        all:*) return 0 ;;
        posix:windows-x64) return 1 ;;
        posix:*) return 0 ;;
        *)
            printf 'Unsupported helper scope: %s\n' "$scope" >&2
            return 2
            ;;
    esac
}


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
GCOV_PROFILE_DIR=
GCOV_PROFILE_PREFIX=
case "$PLATFORM:$CONFIGURATION" in
    linux-*:coverage)
        GCOV_OUTPUT_DIR=$(realpath --relative-to="$ROOT" "$OUT") || exit 1
        GCOV_PROFILE_DIR=$OUT_FINAL
        GCOV_PROFILE_PREFIX="$ROOT/$GCOV_OUTPUT_DIR"
        ;;
    windows-x64:coverage)
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
    scope=$1
    name=$2
    source=$3
    shift 3
    helper_scope_applies "$scope" "$PLATFORM" || {
        status=$?
        [ "$status" -eq 1 ] && return 0
        exit "$status"
    }
    case "$source" in
        "$ROOT"/*) source=${source#"$ROOT"/} ;;
    esac
    compile_args=()
    for compile_arg in "$@"; do
        case "$compile_arg" in
            "$ROOT"/*) compile_args+=("${compile_arg#"$ROOT"/}") ;;
            *) compile_args+=("$compile_arg") ;;
        esac
    done
    output="$OUT/$name$EXE_SUFFIX"
    [ ! -e "$output" ] || {
        printf 'Duplicate test-helper output: %s\n' "$name" >&2
        exit 1
    }
    output_arg=$output
    [ -z "$GCOV_OUTPUT_DIR" ] || output_arg="$GCOV_OUTPUT_DIR/$name$EXE_SUFFIX"
    printf '==> Compiling test helper: %s\n' "$name"
    compile_command=("$CC" "${TEST_CPPFLAGS[@]}" "${TEST_CFLAGS[@]}")
    if [ -n "$GCOV_PROFILE_DIR" ]; then
        compile_command+=(
            "-fprofile-dir=$GCOV_PROFILE_DIR"
            "-fprofile-prefix-path=$GCOV_PROFILE_PREFIX"
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
            "$source" "$coverage_entry_source" "${TEST_LDFLAGS[@]}" \
            "${compile_args[@]}" -o "$output_arg")
    else
        (cd "$ROOT" && "${compile_command[@]}" \
            -I"$DEPS_PREFIX/include" "$source" "${TEST_LDFLAGS[@]}" \
            "${compile_args[@]}" -o "$output_arg")
    fi
}

archive_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
    PKG_CONFIG_SYSROOT_DIR= pkg-config --static --libs libarchive)
compile_helper all archive-fixture "$ROOT/tests/helpers/archive-fixture.c" \
    "$ROOT/src/third_party/sha256.c" -I"$ROOT/include" $archive_libs

event_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
    PKG_CONFIG_SYSROOT_DIR= \
    pkg-config --static --libs libevent_extra libevent_core)
# zlib is a leaf archive in the dependency prefix; no pkg-config metadata is required.
zlib_lib=$(cup_test_find_static_library z) || {
    printf 'zlib static library was not found in %s.\n' "$DEPS_PREFIX" >&2
    exit 1
}
compile_helper all network-helper "$ROOT/tests/helpers/network-helper.c" \
    $event_libs "$zlib_lib" $PLATFORM_LIBS

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
