#!/usr/bin/env bash

# Purpose: Compiles test-only helper programs under Makefile ownership.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
cup_test_require_dependencies
PLATFORM="$CUP_TEST_PLATFORM"
CONFIGURATION="${CUP_TEST_CONFIGURATION:-development}"
case "$PLATFORM" in
    macos-*)
        CC="${CC:-clang}"
        ;;
    *)
        CC="${CC:-gcc}"
        ;;
esac
PLATFORM_LIBS=""
case "$PLATFORM" in
    windows-x64)
        EXE_SUFFIX=.exe
        PLATFORM_LIBS="-lws2_32 -liphlpapi"
        ;;
    *)
        EXE_SUFFIX=
        ;;
esac
OUT="$ROOT/build/$PLATFORM/$CONFIGURATION/tests/helpers"
pkg_path="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig"
COVERAGE_ENTRY_SOURCE=
case "$PLATFORM:$CONFIGURATION" in
    macos-*:coverage)
        COVERAGE_ENTRY_SOURCE="$ROOT/tests/helpers/coverage-entry.c"
        ;;
esac
mkdir -p "$OUT"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O0 -g3"
LDFLAGS=""
case "$PLATFORM" in
    macos-*)
        CFLAGS="$CFLAGS -mmacosx-version-min=13.0"
        LDFLAGS="$LDFLAGS -mmacosx-version-min=13.0"
        ;;
esac
case "$CONFIGURATION" in
    development|debug) ;;
    release)
        CFLAGS="-std=c11 -Wall -Wextra -Werror -O2"
        ;;
    coverage)
        case "$PLATFORM" in
            macos-*)
                CFLAGS="$CFLAGS -fprofile-instr-generate -fcoverage-mapping"
                LDFLAGS="$LDFLAGS -fprofile-instr-generate -fcoverage-mapping"
                ;;
            *)
                CFLAGS="$CFLAGS --coverage -fprofile-arcs -ftest-coverage -fprofile-abs-path"
                LDFLAGS="$LDFLAGS --coverage"
                ;;
        esac
        ;;
    sanitizers)
        CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
        LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
        ;;
    *)
        printf 'Unsupported helper configuration: %s\n' "$CONFIGURATION" >&2
        exit 2
        ;;
esac

compile_helper() {
    name=$1
    source=$2
    shift 2
    printf '==> Compiling test helper: %s\n' "$name"
    if [ -n "$COVERAGE_ENTRY_SOURCE" ]; then
        entry_name=${name//-/_}
        entry_name="cup_coverage_${entry_name}_main"
        "$CC" $CFLAGS \
            "-Dmain=$entry_name" \
            "-DCUP_COVERAGE_ENTRY=$entry_name" \
            -I"$DEPS_PREFIX/include" \
            "$source" "$COVERAGE_ENTRY_SOURCE" $LDFLAGS "$@" \
            -o "$OUT/$name$EXE_SUFFIX"
    else
        "$CC" $CFLAGS -I"$DEPS_PREFIX/include" \
            "$source" $LDFLAGS "$@" \
            -o "$OUT/$name$EXE_SUFFIX"
    fi
}

if [ "$PLATFORM" != windows-x64 ]; then
    archive_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
        PKG_CONFIG_SYSROOT_DIR= pkg-config --static --libs libarchive)
    compile_helper archive-fixture "$ROOT/tests/helpers/archive-fixture.c" \
        $archive_libs
fi

event_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
    PKG_CONFIG_SYSROOT_DIR= \
    pkg-config --static --libs libevent_extra libevent_core)
compile_helper network-helper "$ROOT/tests/helpers/network-helper.c" \
    $event_libs $PLATFORM_LIBS
compile_helper binary-patch "$ROOT/tests/helpers/binary-patch.c"

printf 'All test helpers compiled for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
