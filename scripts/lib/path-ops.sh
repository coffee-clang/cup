#!/bin/sh

# Builds and executes the native filesystem helper used by build tooling.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
SOURCE=$SCRIPT_DIR/path-ops.c
SYSTEM_SOURCE=$PROJECT_ROOT/src/system.c
SYSTEM_POSIX_SOURCE=$PROJECT_ROOT/src/system_posix.c
SYSTEM_WINDOWS_SOURCE=$PROJECT_ROOT/src/system_windows.c
PATH_SOURCE=$PROJECT_ROOT/src/path.c
TEXT_SOURCE=$PROJECT_ROOT/src/text.c
CONSTANTS_HEADER=$PROJECT_ROOT/include/constants.h
ERROR_HEADER=$PROJECT_ROOT/include/error.h
PATH_HEADER=$PROJECT_ROOT/include/path.h
SYSTEM_HEADER=$PROJECT_ROOT/include/system.h
TEXT_HEADER=$PROJECT_ROOT/include/text.h
WINDOWS_UTF_HEADER=$PROJECT_ROOT/include/windows_utf.h
PROTOCOL=3
WINDOWS_WINNT=0x0A00

HOST_SYSTEM=$(uname -s 2>/dev/null || true)
case "${OS:-}:$HOST_SYSTEM" in
    Windows_NT:*|*:MSYS*|*:MINGW*|*:CYGWIN*)
        HOST_MODE=windows-msys
        SYSTEM_PLATFORM_SOURCE=$SYSTEM_WINDOWS_SOURCE
        ;;
    *)
        HOST_MODE=posix
        SYSTEM_PLATFORM_SOURCE=$SYSTEM_POSIX_SOURCE
        ;;
esac

fail() {
    printf 'path ops launcher: %s\n' "$*" >&2
    exit 1
}

valid_sha256() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in
        *[!0-9A-Fa-f]*) return 1 ;;
        *) return 0 ;;
    esac
}

select_sha256_tool() {
    _cup_hash_output=
    _cup_hash_value=
    if command -v sha256sum >/dev/null 2>&1; then
        _cup_hash_output=$(printf '' | sha256sum 2>/dev/null) || _cup_hash_output=
        set -- $_cup_hash_output
        _cup_hash_value=${1:-}
        if valid_sha256 "$_cup_hash_value"; then
            printf '%s\n' sha256sum
            return 0
        fi
    fi
    if command -v shasum >/dev/null 2>&1; then
        _cup_hash_output=$(printf '' | shasum -a 256 2>/dev/null) || _cup_hash_output=
        set -- $_cup_hash_output
        _cup_hash_value=${1:-}
        if valid_sha256 "$_cup_hash_value"; then
            printf '%s\n' shasum
            return 0
        fi
    fi
    fail 'a working sha256sum or shasum is required for the filesystem-helper cache'
}

source_manifest() {
    case "$SHA256_TOOL" in
        sha256sum) sha256sum "$@" 2>/dev/null ;;
        shasum) shasum -a 256 "$@" 2>/dev/null ;;
        *) fail 'invalid SHA-256 tool selection' ;;
    esac
}

stream_sha256() {
    _cup_hash_output=
    case "$SHA256_TOOL" in
        sha256sum) _cup_hash_output=$(sha256sum 2>/dev/null) || _cup_hash_output= ;;
        shasum) _cup_hash_output=$(shasum -a 256 2>/dev/null) || _cup_hash_output= ;;
        *) fail 'invalid SHA-256 tool selection' ;;
    esac
    set -- $_cup_hash_output
    _cup_hash_value=${1:-}
    valid_sha256 "$_cup_hash_value" || fail 'could not hash filesystem-helper cache input'
    printf '%s\n' "$_cup_hash_value"
}

file_owner_and_mode() {
    _cup_stat=$(stat -c '%u %a' "$1" 2>/dev/null) && {
        printf '%s\n' "$_cup_stat"
        return 0
    }
    stat -f '%u %Lp' "$1"
}

safe_cache_mode() {
    _cup_mode=$1
    case "$_cup_mode" in
        0[0-7][0-7][0-7]) _cup_mode=${_cup_mode#0} ;;
        [0-7][0-7][0-7]) ;;
        *) return 1 ;;
    esac

    if [ "$HOST_MODE" = posix ]; then
        [ "$_cup_mode" = 700 ]
        return
    fi

    # MSYS2 projects Windows ACLs into POSIX mode bits and may report 0755
    # after chmod 0700. Read/execute projection is harmless here; shared write
    # access would allow replacement of a cache entry and remains forbidden.
    _cup_shared_mode=${_cup_mode#?}
    case "$_cup_shared_mode" in
        *[2367]*) return 1 ;;
        *) return 0 ;;
    esac
}

owned_private_directory() {
    [ -d "$1" ] && [ ! -L "$1" ] && [ -w "$1" ] && [ -x "$1" ] || return 1
    set -- $(file_owner_and_mode "$1") || return 1
    [ "$1" = "$CURRENT_UID" ] || return 1
    safe_cache_mode "$2"
}

owned_private_helper() {
    _cup_helper_path=$1
    [ -f "$_cup_helper_path" ] && [ ! -L "$_cup_helper_path" ] &&
        [ -x "$_cup_helper_path" ] || return 1
    set -- $(file_owner_and_mode "$_cup_helper_path") || return 1
    [ "$1" = "$CURRENT_UID" ] || return 1
    safe_cache_mode "$2" || return 1
    [ "$("$_cup_helper_path" protocol 2>/dev/null || true)" = "$PROTOCOL" ]
}

RESOLVED_HELPER=
if [ "${1:-}" = --resolved-helper ]; then
    [ "$#" -ge 3 ] || fail '--resolved-helper requires a helper path and command'
    RESOLVED_HELPER=$2
    shift 2
    case "$RESOLVED_HELPER" in
        /*) ;;
        *) fail "resolved helper must be absolute: $RESOLVED_HELPER" ;;
    esac
    CURRENT_UID=$(id -u 2>/dev/null) || fail 'could not determine the current user id'
    if [ "${CUP_PATH_OPS_TESTING:-0}" != 1 ]; then
        case "$RESOLVED_HELPER" in
            /tmp/cup-path-ops-"$CURRENT_UID"/*) ;;
            "${XDG_RUNTIME_DIR:-/nonexistent}"/cup-path-ops-"$CURRENT_UID"/*) ;;
            *) fail "resolved helper is outside the private cache: $RESOLVED_HELPER" ;;
        esac
    fi
    owned_private_helper "$RESOLVED_HELPER" ||
        fail "invalid resolved filesystem helper: $RESOLVED_HELPER"
    HELPER=$RESOLVED_HELPER
fi

if [ -z "$RESOLVED_HELPER" ]; then

for source_file in "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_PLATFORM_SOURCE" \
        "$PATH_SOURCE" "$TEXT_SOURCE" "$CONSTANTS_HEADER" "$ERROR_HEADER" \
        "$PATH_HEADER" "$SYSTEM_HEADER" "$TEXT_HEADER"; do
    [ -f "$source_file" ] && [ ! -L "$source_file" ] ||
        fail "missing helper source: $source_file"
done
if [ "$HOST_MODE" = windows-msys ]; then
    [ -f "$WINDOWS_UTF_HEADER" ] && [ ! -L "$WINDOWS_UTF_HEADER" ] ||
        fail "missing helper source: $WINDOWS_UTF_HEADER"
fi

SHA256_TOOL=$(select_sha256_tool)

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

TEST_COMPILER_OVERRIDE=0
if [ -n "${CUP_PATH_OPS_CC:-}" ]; then
    [ "${CUP_PATH_OPS_TESTING:-0}" = 1 ] ||
        fail 'CUP_PATH_OPS_CC is reserved for repository tests'
    COMPILER=$CUP_PATH_OPS_CC
    TEST_COMPILER_OVERRIDE=1
elif [ -n "${CC_FOR_BUILD:-}" ]; then
    COMPILER=$CC_FOR_BUILD
else
    if [ "$HOST_MODE" = windows-msys ]; then
        case "${MSYSTEM:-}" in
            UCRT64) COMPILER=/ucrt64/bin/gcc ;;
            CLANG64) COMPILER=/clang64/bin/clang ;;
            *) fail 'Windows filesystem helper requires an MSYS2 UCRT64 or CLANG64 environment' ;;
        esac
    else
        COMPILER=
        for candidate in cc gcc clang; do
            if command -v "$candidate" >/dev/null 2>&1; then
                COMPILER=$candidate
                break
            fi
        done
    fi
fi
[ -n "$COMPILER" ] && command -v "$COMPILER" >/dev/null 2>&1 ||
    fail "a build-host C compiler is required (${COMPILER:-cc, gcc or clang})"
COMPILER_PATH=$(command -v "$COMPILER") || fail "could not resolve compiler: $COMPILER"
COMPILER_VERSION=$("$COMPILER_PATH" --version 2>&1 | sed -n '1p')
[ -n "$COMPILER_VERSION" ] || fail "could not identify compiler: $COMPILER_PATH"

if [ "$HOST_MODE" = windows-msys ]; then
    if [ "$TEST_COMPILER_OVERRIDE" -eq 0 ]; then
        case "$COMPILER_PATH" in
            /ucrt64/bin/*|/clang64/bin/*) ;;
            *)
                fail "Windows filesystem helper compiler is outside a native UCRT toolchain: $COMPILER_PATH"
                ;;
        esac
    fi
    HOST_FLAGS="native-windows-ucrt;winnt=$WINDOWS_WINNT"
    HOST_LIBS='-lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi -lsecur32'
else
    HOST_FLAGS=-U_WIN32
    HOST_LIBS=
fi

CURRENT_UID=$(id -u 2>/dev/null) || fail 'could not determine the current user id'
if [ "$HOST_MODE" = windows-msys ]; then
    SOURCE_MANIFEST=$(source_manifest \
        "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_PLATFORM_SOURCE" \
        "$PATH_SOURCE" "$TEXT_SOURCE" "$CONSTANTS_HEADER" "$ERROR_HEADER" \
        "$PATH_HEADER" "$SYSTEM_HEADER" "$TEXT_HEADER" "$WINDOWS_UTF_HEADER") ||
        fail 'could not hash filesystem-helper sources'
else
    SOURCE_MANIFEST=$(source_manifest \
        "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_PLATFORM_SOURCE" \
        "$PATH_SOURCE" "$TEXT_SOURCE" "$CONSTANTS_HEADER" "$ERROR_HEADER" \
        "$PATH_HEADER" "$SYSTEM_HEADER" "$TEXT_HEADER") ||
        fail 'could not hash filesystem-helper sources'
fi
BUILD_ID=$(
    {
        printf '%s\n' \
            "protocol=$PROTOCOL" \
            "host_mode=$HOST_MODE" \
            "compiler=$COMPILER_PATH" \
            "compiler_version=$COMPILER_VERSION" \
            "host_flags=$HOST_FLAGS" \
            "host_libs=$HOST_LIBS" \
            'flags=-std=c11 -O2 -Wall -Wextra -Werror'
        printf '%s\n' "$SOURCE_MANIFEST"
    } | stream_sha256
)

HOST_ID=$(printf '%s' "$HOST_SYSTEM" | tr -cd 'A-Za-z0-9_.-' || true)
MACHINE_ID=$(uname -m 2>/dev/null | tr -cd 'A-Za-z0-9_.-' || true)
[ -n "$HOST_ID" ] || HOST_ID=unknown
[ -n "$MACHINE_ID" ] || MACHINE_ID=unknown

CACHE_BASE=/tmp
if [ -n "${XDG_RUNTIME_DIR:-}" ] && owned_private_directory "$XDG_RUNTIME_DIR"; then
    CACHE_BASE=$XDG_RUNTIME_DIR
fi
CACHE_ROOT=$CACHE_BASE/cup-path-ops-$CURRENT_UID
if [ "$HOST_MODE" = windows-msys ]; then
    HELPER_SUFFIX=.exe
else
    HELPER_SUFFIX=
fi
HELPER=$CACHE_ROOT/path-ops-$HOST_ID-$MACHINE_ID-$BUILD_ID$HELPER_SUFFIX

umask 077
if [ ! -e "$CACHE_ROOT" ] && [ ! -L "$CACHE_ROOT" ]; then
    mkdir -m 0700 "$CACHE_ROOT" 2>/dev/null || true
fi
owned_private_directory "$CACHE_ROOT" || fail "unsafe helper cache: $CACHE_ROOT"

if [ -e "$HELPER" ] || [ -L "$HELPER" ]; then
    owned_private_helper "$HELPER" ||
        fail "invalid cached helper was preserved: $HELPER"
else
    if [ "$HOST_MODE" = windows-msys ]; then
        TEMPORARY=$(mktemp --suffix=.exe "$CACHE_ROOT/.path-ops.XXXXXX") ||
            fail 'could not allocate helper output'
    else
        TEMPORARY=$(mktemp "$CACHE_ROOT/.path-ops.XXXXXX") ||
            fail 'could not allocate helper output'
    fi
    cleanup() { rm -f -- "$TEMPORARY"; }
    trap cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ "$HOST_MODE" = windows-msys ]; then
        # Use the native Windows backend so helper filesystem semantics match cup.
        "$COMPILER_PATH" -std=c11 -O2 -Wall -Wextra -Werror \
            "-D_WIN32_WINNT=$WINDOWS_WINNT" "-DWINVER=$WINDOWS_WINNT" \
            -I"$PROJECT_ROOT/include" \
            "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_WINDOWS_SOURCE" \
            "$PATH_SOURCE" "$TEXT_SOURCE" \
            $HOST_LIBS -o "$TEMPORARY" ||
            fail 'could not build native Windows filesystem helper'
    else
        "$COMPILER_PATH" -std=c11 -O2 -Wall -Wextra -Werror -U_WIN32 \
            -I"$PROJECT_ROOT/include" \
            "$SOURCE" "$SYSTEM_SOURCE" "$SYSTEM_POSIX_SOURCE" \
            "$PATH_SOURCE" "$TEXT_SOURCE" -o "$TEMPORARY" ||
            fail 'could not build filesystem helper'
    fi
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
fi

if [ "$HOST_MODE" = windows-msys ]; then
    command -v cygpath >/dev/null 2>&1 ||
        fail 'cygpath is required at the MSYS/native filesystem boundary'

    shell_path_check() {
        MSYS2_ARG_CONV_EXCL='*' "$HELPER" check-shell-absolute "$1" >/dev/null
    }

    native_path() {
        _cup_shell_path=$1
        shell_path_check "$_cup_shell_path" ||
            fail "path is not a clean absolute shell path: $_cup_shell_path"

        # Convert only the MSYS root mapping. Converting the complete path with
        # cygpath would resolve junctions before the native helper can enforce
        # its component-by-component no-follow policy.
        case "$_cup_shell_path" in
            /[A-Za-z]/*)
                _cup_drive=${_cup_shell_path#/}
                _cup_drive=${_cup_drive%%/*}
                _cup_native_root=$(cygpath -m -- "/$_cup_drive") ||
                    fail "could not convert MSYS drive root: /$_cup_drive"
                _cup_remainder=${_cup_shell_path#"/$_cup_drive/"}
                ;;
            /*)
                _cup_native_root=$(cygpath -m -- /) ||
                    fail 'could not convert the MSYS filesystem root'
                _cup_remainder=${_cup_shell_path#/}
                ;;
        esac
        printf '%s/%s\n' "${_cup_native_root%/}" "$_cup_remainder"
    }

    run_native_helper() {
        # Every filesystem operand has already been converted deliberately. Keep
        # MSYS from rewriting arguments or path-valued environment variables a
        # second time while the native helper owns the operation.
        MSYS2_ARG_CONV_EXCL='*' MSYS2_ENV_CONV_EXCL='*' "$HELPER" "$@"
    }

    exec_native_helper() {
        MSYS2_ARG_CONV_EXCL='*' MSYS2_ENV_CONV_EXCL='*' exec "$HELPER" "$@"
    }

    case "${1:-}" in
        check-shell-absolute|check-shell-relative|check-shell-within)
            # These commands validate shell spelling itself, so preserve the
            # original arguments byte-for-byte.
            MSYS2_ARG_CONV_EXCL='*' exec "$HELPER" "$@"
            ;;
        check-dir)
            [ "$#" -eq 2 ] || { [ "$#" -eq 3 ] && [ "$3" = allow-missing ]; } ||
                fail 'check-dir requires a path and optional allow-missing'
            NATIVE_PATH=$(native_path "$2")
            if [ "$#" -eq 3 ]; then
                exec_native_helper check-dir "$NATIVE_PATH" allow-missing
            fi
            exec_native_helper check-dir "$NATIVE_PATH"
            ;;
        ensure-dir|mkdir-exclusive|check-file|check-tree|remove-tree|rmdir|remove-file|check-build-root|prepare-build-root|clean-build-root)
            [ "$#" -eq 2 ] || fail "$1 requires one path"
            NATIVE_PATH=$(native_path "$2")
            exec_native_helper "$1" "$NATIVE_PATH"
            ;;
        mkdir-unique)
            [ "$#" -eq 3 ] || fail 'mkdir-unique requires a template and mode'
            NATIVE_PATH=$(native_path "$2")
            NATIVE_UNIQUE=$(run_native_helper mkdir-unique "$NATIVE_PATH" "$3") || exit $?
            cygpath -u "$NATIVE_UNIQUE" ||
                fail "could not convert unique directory to MSYS form: $NATIVE_UNIQUE"
            exit 0
            ;;
        copy-file)
            [ "$#" -eq 4 ] || [ "$#" -eq 5 ] ||
                fail 'copy-file requires source, destination, mode and optional policy'
            NATIVE_SOURCE=$(native_path "$2")
            NATIVE_DESTINATION=$(native_path "$3")
            if [ "$#" -eq 5 ]; then
                exec_native_helper copy-file "$NATIVE_SOURCE" "$NATIVE_DESTINATION" "$4" "$5"
            fi
            exec_native_helper copy-file "$NATIVE_SOURCE" "$NATIVE_DESTINATION" "$4"
            ;;
        copy-tree|move)
            [ "$#" -eq 3 ] || fail "$1 requires source and destination"
            NATIVE_SOURCE=$(native_path "$2")
            NATIVE_DESTINATION=$(native_path "$3")
            exec_native_helper "$1" "$NATIVE_SOURCE" "$NATIVE_DESTINATION"
            ;;
        write-stdin)
            [ "$#" -eq 3 ] || [ "$#" -eq 4 ] ||
                fail 'write-stdin requires destination, mode and optional policy'
            NATIVE_PATH=$(native_path "$2")
            if [ "$#" -eq 4 ]; then
                exec_native_helper write-stdin "$NATIVE_PATH" "$3" "$4"
            fi
            exec_native_helper write-stdin "$NATIVE_PATH" "$3"
            ;;
        run-build|run-lock)
            [ "$#" -ge 4 ] && [ "${3:-}" = -- ] ||
                fail "$1 requires a path, -- and a command"
            COMMAND=$1
            NATIVE_PATH=$(native_path "$2")
            shift 3
            exec_native_helper "$COMMAND" "$NATIVE_PATH" -- "$@"
            ;;
        protocol)
            exec "$HELPER" "$@"
            ;;
        *)
            fail "unsupported Windows filesystem-helper command: ${1:-missing}"
            ;;
    esac
fi

exec "$HELPER" "$@"
