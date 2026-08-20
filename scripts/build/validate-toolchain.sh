#!/bin/sh

# Rejects platform/compiler combinations that cannot produce the
# requested native cup executable before any source file is compiled.
set -eu

platform=${1:?platform is required}
compiler=${2:?compiler is required}
windres=${3:-}
configuration=${4:-development}
role=${5:-primary}

fail() {
    printf 'toolchain: %s\n' "$*" >&2
    exit 1
}

case "$configuration" in
    development|debug|coverage|sanitizers|release) ;;
    *) fail "unsupported build configuration '$configuration'" ;;
esac
case "$role" in
    primary|secondary) ;;
    *) fail "unsupported toolchain role '$role'" ;;
esac
[ "$role" = primary ] || [ "$configuration" = development ] ||
    fail 'secondary toolchains are supported only for development builds'

set -- $compiler
compiler_program=${1:-}
[ -n "$compiler_program" ] || fail 'empty compiler command'
compiler_path=$(command -v "$compiler_program" 2>/dev/null || true)
[ -n "$compiler_path" ] || fail "compiler '$compiler_program' was not found"

target=$($compiler -dumpmachine 2>/dev/null || $compiler -print-target-triple 2>/dev/null) ||
    fail "compiler '$compiler' does not report a target triple"
[ -n "$target" ] || fail "compiler '$compiler' reported an empty target triple"
compiler_id=$($compiler --version 2>/dev/null | sed -n '1p')
[ -n "$compiler_id" ] || fail "compiler '$compiler' does not report a version"

host_system=$(uname -s 2>/dev/null || printf unknown)
host_machine=$(uname -m 2>/dev/null || printf unknown)

require_gcc() {
    case "$compiler_id" in
        *gcc*|*GCC*) ;;
        *) fail "$platform $configuration requires GCC, got: $compiler_id" ;;
    esac
}

require_clang() {
    case "$compiler_id" in
        *clang*|*Clang*) ;;
        *) fail "$platform $configuration requires Clang, got: $compiler_id" ;;
    esac
}

case "$platform" in
    linux-x64)
        case "$host_system:$host_machine" in
            Linux:x86_64|Linux:amd64) ;;
            *) fail "PLATFORM '$platform' requires native Linux x64, got $host_system/$host_machine" ;;
        esac
        case "$target" in
            x86_64*-linux*|amd64*-linux*) ;;
            *) fail "compiler target '$target' does not match $platform" ;;
        esac
        case "$configuration:$role" in
            sanitizers:primary) require_clang ;;
            development:secondary) require_clang ;;
            *) require_gcc ;;
        esac
        ;;
    linux-arm64)
        case "$host_system:$host_machine" in
            Linux:aarch64|Linux:arm64) ;;
            *) fail "PLATFORM '$platform' requires native Linux ARM64, got $host_system/$host_machine" ;;
        esac
        case "$target" in
            aarch64*-linux*|arm64*-linux*) ;;
            *) fail "compiler target '$target' does not match $platform" ;;
        esac
        case "$configuration:$role" in
            sanitizers:primary) require_clang ;;
            development:secondary) require_clang ;;
            *) require_gcc ;;
        esac
        ;;
    macos-x64)
        case "$host_system:$host_machine" in
            Darwin:x86_64|Darwin:amd64) ;;
            *) fail "PLATFORM '$platform' requires native macOS x64, got $host_system/$host_machine" ;;
        esac
        case "$target" in
            x86_64*-apple-darwin*|amd64*-apple-darwin*) ;;
            *) fail "compiler target '$target' does not match $platform" ;;
        esac
        require_clang
        case "$compiler_id" in
            *Apple*clang*|*Apple*Clang*) ;;
            *) fail "macOS builds require Apple Clang, got: $compiler_id" ;;
        esac
        ;;
    macos-arm64)
        case "$host_system:$host_machine" in
            Darwin:arm64|Darwin:aarch64) ;;
            *) fail "PLATFORM '$platform' requires native macOS ARM64, got $host_system/$host_machine" ;;
        esac
        case "$target" in
            arm64*-apple-darwin*|aarch64*-apple-darwin*) ;;
            *) fail "compiler target '$target' does not match $platform" ;;
        esac
        require_clang
        case "$compiler_id" in
            *Apple*clang*|*Apple*Clang*) ;;
            *) fail "macOS builds require Apple Clang, got: $compiler_id" ;;
        esac
        ;;
    windows-x64)
        case "$host_system:$host_machine" in
            MINGW*:x86_64|MSYS*:x86_64) ;;
            *) fail "PLATFORM '$platform' requires native MSYS2 x64, got $host_system/$host_machine" ;;
        esac
        [ "${MSYSTEM_CARCH:-x86_64}" = x86_64 ] || fail 'windows-x64 requires MSYSTEM_CARCH=x86_64'
        case "$target" in
            x86_64-w64-mingw32|x86_64*-windows-gnu) ;;
            *) fail "compiler target '$target' does not match $platform" ;;
        esac

        case "$configuration" in
            sanitizers)
                [ "${MSYSTEM:-}" = CLANG64 ] || fail 'windows-x64 sanitizers require MSYSTEM=CLANG64'
                [ "${MINGW_PREFIX:-}" = /clang64 ] || fail 'windows-x64 sanitizers require MINGW_PREFIX=/clang64'
                require_clang
                expected_prefix=/clang64/bin/
                ;;
            *)
                [ "${MSYSTEM:-}" = UCRT64 ] || fail "windows-x64 $configuration requires MSYSTEM=UCRT64"
                [ "${MINGW_PREFIX:-}" = /ucrt64 ] || fail "windows-x64 $configuration requires MINGW_PREFIX=/ucrt64"
                require_gcc
                expected_prefix=/ucrt64/bin/
                ;;
        esac
        case "$compiler_path" in
            "$expected_prefix"*) ;;
            *) fail "compiler is outside the selected MSYS2 toolchain: $compiler_path" ;;
        esac

        [ -n "$windres" ] || fail 'WINDRES is required for windows-x64'
        set -- $windres
        windres_program=${1:-}
        windres_path=$(command -v "$windres_program" 2>/dev/null || true)
        [ -n "$windres_path" ] || fail "resource compiler '$windres_program' was not found"
        case "$windres_path" in
            "$expected_prefix"*) ;;
            *) fail "resource compiler is outside the selected MSYS2 toolchain: $windres_path" ;;
        esac
        if [ "$configuration" = sanitizers ]; then
            case "$(basename "$windres_program")" in
                llvm-windres|llvm-windres.exe) ;;
                *) fail 'windows-x64 sanitizers require llvm-windres' ;;
            esac
        fi
        ;;
    *) fail "unsupported platform '$platform'" ;;
esac
