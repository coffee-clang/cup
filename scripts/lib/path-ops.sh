#!/bin/sh

# Builds and executes the fd-relative filesystem helper used by build tooling.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
SOURCE=$SCRIPT_DIR/path-ops.c
SYSTEM_SOURCE=$PROJECT_ROOT/src/system.c
SYSTEM_POSIX_SOURCE=$PROJECT_ROOT/src/system_posix.c
PATH_SOURCE=$PROJECT_ROOT/src/path.c
TEXT_SOURCE=$PROJECT_ROOT/src/text.c
PROTOCOL=2

fail() {
    printf 'path ops launcher: %s\n' "$*" >&2
    exit 1
}

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail 'sha256sum or shasum is required for the filesystem-helper cache'
    fi
}

stream_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{print $1}'
    else
        fail 'sha256sum or shasum is required for the filesystem-helper cache'
    fi
}

file_owner_and_mode() {
    if stat -c '%u %a' "$1" >/dev/null 2>&1; then
        stat -c '%u %a' "$1"
    else
        stat -f '%u %Lp' "$1"
    fi
}

owned_private_directory() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 1
    set -- $(file_owner_and_mode "$1") || return 1
    [ "$1" = "$CURRENT_UID" ] || return 1
    case "$2" in
        700|0700) return 0 ;;
        *) return 1 ;;
    esac
}

owned_private_helper() {
    _cup_helper_path=$1
    [ -f "$_cup_helper_path" ] && [ ! -L "$_cup_helper_path" ] &&
        [ -x "$_cup_helper_path" ] || return 1
    set -- $(file_owner_and_mode "$_cup_helper_path") || return 1
    [ "$1" = "$CURRENT_UID" ] || return 1
    case "$2" in
        700|0700) ;;
        *) return 1 ;;
    esac
    [ "$("$_cup_helper_path" protocol 2>/dev/null || true)" = "$PROTOCOL" ]
}

for source_file in "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_POSIX_SOURCE" \
        "$PATH_SOURCE" "$TEXT_SOURCE"; do
    [ -f "$source_file" ] && [ ! -L "$source_file" ] ||
        fail "missing helper source: $source_file"
done

PRINT_HELPER=0
if [ "${1:-}" = --print-helper ]; then
    PRINT_HELPER=1
    shift
fi

if [ -n "${CUP_PATH_OPS_HELPER:-}" ]; then
    [ "${CUP_PATH_OPS_TESTING:-0}" = 1 ] ||
        fail 'CUP_PATH_OPS_HELPER is reserved for repository tests'
    HELPER=$CUP_PATH_OPS_HELPER
    case "$HELPER" in /*) ;; *) fail "helper override must be absolute: $HELPER" ;; esac
    CURRENT_UID=$(id -u 2>/dev/null) || fail 'could not determine the current user id'
    owned_private_helper "$HELPER" || fail "invalid CUP_PATH_OPS_HELPER: $HELPER"
    if [ "$PRINT_HELPER" -eq 1 ]; then
        printf '%s\n' "$HELPER"
        exit 0
    fi
    exec "$HELPER" "$@"
fi

if [ -n "${CUP_PATH_OPS_CC:-}" ]; then
    [ "${CUP_PATH_OPS_TESTING:-0}" = 1 ] ||
        fail 'CUP_PATH_OPS_CC is reserved for repository tests'
    COMPILER=$CUP_PATH_OPS_CC
elif [ -n "${CC_FOR_BUILD:-}" ]; then
    COMPILER=$CC_FOR_BUILD
else
    case "${OS:-}:$(uname -s 2>/dev/null || true)" in
        Windows_NT:*|*:MSYS*|*:MINGW*|*:CYGWIN*) COMPILER=/usr/bin/gcc ;;
        *)
            COMPILER=
            for candidate in cc gcc clang; do
                if command -v "$candidate" >/dev/null 2>&1; then
                    COMPILER=$candidate
                    break
                fi
            done
            ;;
    esac
fi
[ -n "$COMPILER" ] && command -v "$COMPILER" >/dev/null 2>&1 ||
    fail "a build-host C compiler is required (${COMPILER:-cc, gcc or clang})"
COMPILER_PATH=$(command -v "$COMPILER") || fail "could not resolve compiler: $COMPILER"
COMPILER_VERSION=$("$COMPILER_PATH" --version 2>&1 | sed -n '1p')
[ -n "$COMPILER_VERSION" ] || fail "could not identify compiler: $COMPILER_PATH"

HOST_CPPFLAGS=
case "${OS:-}:$(uname -s 2>/dev/null || true)" in
    Windows_NT:*|*:MSYS*|*:MINGW*|*:CYGWIN*)
        HOST_CPPFLAGS=-D_POSIX_C_SOURCE=200809L
        ;;
esac

CURRENT_UID=$(id -u 2>/dev/null) || fail 'could not determine the current user id'
SOURCE_ID=$(
    for source_file in "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_POSIX_SOURCE" \
            "$PATH_SOURCE" "$TEXT_SOURCE"; do
        printf '%s=%s\n' "$source_file" "$(file_sha256 "$source_file")"
    done | stream_sha256
)
BUILD_ID=$(printf '%s\n' \
    "protocol=$PROTOCOL" \
    "sources=$SOURCE_ID" \
    "compiler=$COMPILER_PATH" \
    "compiler_version=$COMPILER_VERSION" \
    "host_cppflags=$HOST_CPPFLAGS" \
    'flags=-std=c11 -O2 -Wall -Wextra -Werror -U_WIN32' | stream_sha256)
HOST_ID=$(uname -s 2>/dev/null | tr -cd 'A-Za-z0-9_.-' || true)
MACHINE_ID=$(uname -m 2>/dev/null | tr -cd 'A-Za-z0-9_.-' || true)
[ -n "$HOST_ID" ] || HOST_ID=unknown
[ -n "$MACHINE_ID" ] || MACHINE_ID=unknown

CACHE_BASE=/tmp
if [ -n "${XDG_RUNTIME_DIR:-}" ] && owned_private_directory "$XDG_RUNTIME_DIR"; then
    CACHE_BASE=$XDG_RUNTIME_DIR
fi
CACHE_ROOT=$CACHE_BASE/cup-path-ops-$CURRENT_UID
HELPER=$CACHE_ROOT/path-ops-$HOST_ID-$MACHINE_ID-$BUILD_ID

umask 077
if [ ! -e "$CACHE_ROOT" ] && [ ! -L "$CACHE_ROOT" ]; then
    mkdir -m 0700 "$CACHE_ROOT" 2>/dev/null || true
fi
owned_private_directory "$CACHE_ROOT" || fail "unsafe helper cache: $CACHE_ROOT"

if [ -e "$HELPER" ] || [ -L "$HELPER" ]; then
    owned_private_helper "$HELPER" ||
        fail "invalid cached helper was preserved: $HELPER"
else
    TEMPORARY=$(mktemp "$CACHE_ROOT/.path-ops.XXXXXX") || fail 'could not allocate helper output'
    cleanup() { rm -f -- "$TEMPORARY"; }
    trap cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    "$COMPILER_PATH" ${HOST_CPPFLAGS:+"$HOST_CPPFLAGS"} \
        -std=c11 -O2 -Wall -Wextra -Werror -U_WIN32 \
        -I"$PROJECT_ROOT/include" \
        "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_POSIX_SOURCE" \
        "$PATH_SOURCE" "$TEXT_SOURCE" -o "$TEMPORARY" ||
        fail 'could not build filesystem helper'
    chmod 0700 "$TEMPORARY"
    owned_private_helper "$TEMPORARY" || fail 'new filesystem helper failed validation'
    if ln "$TEMPORARY" "$HELPER" 2>/dev/null; then
        rm -f -- "$TEMPORARY"
    else
        owned_private_helper "$HELPER" || fail 'could not publish filesystem helper'
    fi
    trap - EXIT HUP INT TERM
fi

owned_private_helper "$HELPER" || fail "invalid cached helper: $HELPER"
if [ "$PRINT_HELPER" -eq 1 ]; then
    printf '%s\n' "$HELPER"
    exit 0
fi
exec "$HELPER" "$@"
