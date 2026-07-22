#!/bin/sh

# Purpose: Rejects platform/compiler combinations that cannot produce the
# requested CUP executable before any source file is compiled.
set -eu

platform=${1:?platform is required}
compiler=${2:?compiler is required}
windres=${3:-}

fail() {
    printf 'toolchain: %s\n' "$*" >&2
    exit 1
}

# Compiler commands may contain simple launcher arguments. Whitespace inside
# the compiler executable path remains outside the command-string contract; this
# does not restrict the source checkout path.
set -- $compiler
compiler_program=${1:-}
[ -n "$compiler_program" ] || fail 'empty compiler command'
command -v "$compiler_program" >/dev/null 2>&1 ||
    fail "compiler '$compiler_program' was not found"

target=$($compiler -dumpmachine 2>/dev/null) ||
    fail "compiler '$compiler' does not report a target triple"
[ -n "$target" ] || fail "compiler '$compiler' reported an empty target triple"

host_system=$(uname -s 2>/dev/null || printf unknown)
host_machine=$(uname -m 2>/dev/null || printf unknown)

case "$platform" in
    linux-x64)
        case "$host_system:$host_machine" in
            Linux:x86_64|Linux:amd64)
                ;;
            *)
                fail "PLATFORM '$platform' requires a native Linux x64 host, got $host_system/$host_machine"
                ;;
        esac
        case "$target" in
            x86_64*-linux*|amd64*-linux*)
                ;;
            *)
                fail "compiler target '$target' does not match $platform"
                ;;
        esac
        ;;
    linux-arm64)
        case "$host_system:$host_machine" in
            Linux:aarch64|Linux:arm64)
                ;;
            *)
                fail "PLATFORM '$platform' requires a native Linux ARM64 host, got $host_system/$host_machine"
                ;;
        esac
        case "$target" in
            aarch64*-linux*|arm64*-linux*)
                ;;
            *)
                fail "compiler target '$target' does not match $platform"
                ;;
        esac
        ;;
    macos-x64)
        case "$host_system:$host_machine" in
            Darwin:x86_64|Darwin:amd64)
                ;;
            *)
                fail "PLATFORM '$platform' requires a native macOS x64 host, got $host_system/$host_machine"
                ;;
        esac
        case "$target" in
            x86_64*-apple-darwin*|amd64*-apple-darwin*)
                ;;
            *)
                fail "compiler target '$target' does not match $platform"
                ;;
        esac
        ;;
    macos-arm64)
        case "$host_system:$host_machine" in
            Darwin:arm64|Darwin:aarch64)
                ;;
            *)
                fail "PLATFORM '$platform' requires a native macOS ARM64 host, got $host_system/$host_machine"
                ;;
        esac
        case "$target" in
            arm64*-apple-darwin*|aarch64*-apple-darwin*)
                ;;
            *)
                fail "compiler target '$target' does not match $platform"
                ;;
        esac
        ;;
    windows-x64)
        case "$host_system:$host_machine" in
            MINGW*:x86_64|MSYS*:x86_64)
                ;;
            *)
                fail "PLATFORM '$platform' requires a native MSYS2 UCRT64 x64 host, got $host_system/$host_machine"
                ;;
        esac
        [ "${MSYSTEM:-}" = UCRT64 ] ||
            fail "PLATFORM '$platform' requires MSYSTEM=UCRT64"
        [ "${MINGW_PREFIX:-}" = /ucrt64 ] ||
            fail "PLATFORM '$platform' requires MINGW_PREFIX=/ucrt64"
        case "${MSYSTEM_CARCH:-x86_64}" in
            x86_64)
                ;;
            *)
                fail "PLATFORM '$platform' requires MSYSTEM_CARCH=x86_64"
                ;;
        esac
        case "$target" in
            x86_64-w64-mingw32|x86_64*-windows-gnu)
                ;;
            *)
                fail "compiler target '$target' does not match the UCRT64 policy for $platform"
                ;;
        esac
        [ -n "$windres" ] || fail 'WINDRES is required for windows-x64'
        set -- $windres
        windres_program=${1:-}
        command -v "$windres_program" >/dev/null 2>&1 ||
            fail "resource compiler '$windres_program' was not found"
        ;;
    *)
        fail "unsupported platform '$platform'"
        ;;
esac
