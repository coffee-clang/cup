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
    macos-*) CC="${CC:-clang}" ;;
    *) CC="${CC:-gcc}" ;;
esac
case "$PLATFORM" in
    windows-x64) EXE_SUFFIX=.exe ;;
    *) EXE_SUFFIX= ;;
esac
OUT="$ROOT/build/$PLATFORM/$CONFIGURATION/tests/helpers"
mkdir -p "$OUT"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O0 -g3"
LDFLAGS=""
case "$CONFIGURATION" in
    development|debug) ;;
    release) CFLAGS="-std=c11 -Wall -Wextra -Werror -O2" ;;
    coverage)
        CFLAGS="$CFLAGS --coverage -fprofile-arcs -ftest-coverage"
        LDFLAGS="$LDFLAGS --coverage"
        ;;
    sanitizers)
        CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
        LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
        ;;
    *) printf 'Unsupported helper configuration: %s\n' "$CONFIGURATION" >&2; exit 2 ;;
esac

if [ "$PLATFORM" != windows-x64 ]; then
    printf '==> Compiling test helper: archive-fixture\n'
    pkg_path="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig"
    archive_libs=$(PKG_CONFIG_PATH="$pkg_path" PKG_CONFIG_LIBDIR="$pkg_path" \
        PKG_CONFIG_SYSROOT_DIR= pkg-config --static --libs libarchive)
    "$CC" $CFLAGS -I"$DEPS_PREFIX/include" \
        "$ROOT/tests/helpers/archive-fixture.c" $LDFLAGS $archive_libs \
        -o "$OUT/archive-fixture$EXE_SUFFIX"
fi

printf '==> Compiling test helper: http-server\n'
http_libs=
[ "$PLATFORM" != windows-x64 ] || http_libs='-lws2_32'
"$CC" $CFLAGS "$ROOT/tests/helpers/http-server.c" $LDFLAGS $http_libs \
    -o "$OUT/http-server$EXE_SUFFIX"

if [ "$PLATFORM" != windows-x64 ]; then
    printf '==> Compiling test helper: connect-proxy\n'
    "$CC" $CFLAGS -D_POSIX_C_SOURCE=200809L \
        "$ROOT/tests/helpers/connect-proxy.c" $LDFLAGS \
        -o "$OUT/connect-proxy"
fi
printf 'All test helpers compiled for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
