#!/usr/bin/env bash

# Purpose: Validates a complete pinned dependency prefix and its canonical
# format-3 identity before any CUP build consumes it.
set -euo pipefail

PLATFORM=${1:?platform is required}
MODE=${2:?dependency prefix or --print-id is required}
DEPS_PREFIX=
if [ "$MODE" != --print-id ]; then
    DEPS_PREFIX=$MODE
fi
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
require_sha256_tool

case "$PLATFORM" in
    linux-x64)
        CC=gcc
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=linux-x86_64
        platform_policy=glibc-native
        use_openssl=1
        builder=build-posix.sh
        ;;
    linux-arm64)
        CC=gcc
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=linux-aarch64
        platform_policy=glibc-native
        use_openssl=1
        builder=build-posix.sh
        ;;
    macos-x64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-x86_64-cc
        platform_policy=macos-deployment-13.0
        use_openssl=1
        builder=build-posix.sh
        ;;
    macos-arm64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-arm64-cc
        platform_policy=macos-deployment-13.0
        use_openssl=1
        builder=build-posix.sh
        ;;
    windows-x64)
        host_triple=x86_64-w64-mingw32
        case "${MSYSTEM:-}" in
            UCRT64)
                [ "${MINGW_PREFIX:-}" = /ucrt64 ] || exit 1
                CC=gcc; AR=ar; RANLIB=ranlib; STRIP=strip; WINDRES=windres
                runtime_policy=msys2-ucrt64
                ;;
            CLANG64)
                [ "${MINGW_PREFIX:-}" = /clang64 ] || exit 1
                CC=clang; AR=llvm-ar; RANLIB=llvm-ranlib
                STRIP=llvm-strip; WINDRES=llvm-windres
                runtime_policy=msys2-clang64
                ;;
            *)
                echo "Windows dependencies require an MSYS2 UCRT64 or CLANG64 shell." >&2
                exit 1
                ;;
        esac
        use_openssl=0
        builder=build-windows.sh
        ;;
    *)
        echo "Unsupported dependency platform: $PLATFORM" >&2
        exit 1
        ;;
esac

if [ "$use_openssl" = 1 ]; then
    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    toolchain=$(dependency_posix_toolchain_identity \
        "$CC" "$AR" "$RANLIB" "$OPENSSL_TARGET" "$platform_policy")
else
    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_tool "$STRIP"
    require_tool "$WINDRES"
    compiler_target=$("$CC" -dumpmachine)
    case "$compiler_target" in
        x86_64-w64-mingw32|x86_64-w64-windows-gnu) ;;
        *) echo "Windows compiler target '$compiler_target' is unsupported." >&2; exit 1 ;;
    esac
    toolchain=$(dependency_windows_toolchain_identity \
        "$host_triple" "$CC" "$AR" "$RANLIB" "$STRIP" "$WINDRES" \
        "$runtime_policy")
fi

id=$(dependency_id "$PLATFORM" "$toolchain" "$use_openssl" "$PROJECT_ROOT" \
    "$SCRIPT_DIR/sources.sh" \
    "$SCRIPT_DIR/common.sh" \
    "$SCRIPT_DIR/$builder")
if [ "$MODE" = --print-id ]; then
    printf '%s\n' "$id"
    exit 0
fi

metadata=$(dependency_metadata "$PLATFORM" "$id")

dependency_prefix_matches "$DEPS_PREFIX" "$metadata" "$use_openssl" || {
    echo "Pinned dependency prefix is missing, incomplete or stale: $DEPS_PREFIX" >&2
    echo "Expected dependency_id: $id" >&2
    exit 1
}
