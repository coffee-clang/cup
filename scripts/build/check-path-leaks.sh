#!/usr/bin/env sh

# Purpose: Rejects machine-specific and transactional paths in release binaries.
set -eu

binary=${1:?binary is required}
shift

if ! command -v strings >/dev/null 2>&1; then
    echo "Error: strings is required to inspect release binaries." >&2
    exit 2
fi

strings_file=$(mktemp "${TMPDIR:-/tmp}/cup-release-strings.XXXXXX")
trap 'rm -f "$strings_file"' EXIT HUP INT TERM
strings -a "$binary" >"$strings_file"

if grep -E -q \
    '/\.[^/]*install\.staging\.|[A-Za-z]:[\\/].*[\\/]\.[^\\/]*install\.staging\.' \
    "$strings_file"; then
    echo "Error: release binary contains a transactional dependency path." >&2
    exit 1
fi

check_forbidden_path() {
    candidate=$1

    [ -n "$candidate" ] || return 0
    if grep -F -q -- "$candidate" "$strings_file"; then
        echo "Error: release binary contains forbidden path: $candidate" >&2
        return 1
    fi
}

for path in "$@"; do
    [ -n "$path" ] || continue
    case "$path" in
        / | .)
            continue
            ;;
    esac

    check_forbidden_path "$path" || exit 1
    if command -v cygpath >/dev/null 2>&1; then
        mixed=$(cygpath -m "$path" 2>/dev/null || true)
        windows=$(cygpath -w "$path" 2>/dev/null || true)
        [ "$mixed" = "$path" ] || check_forbidden_path "$mixed" || exit 1
        [ "$windows" = "$path" ] || [ "$windows" = "$mixed" ] || \
            check_forbidden_path "$windows" || exit 1
    fi
done

# OpenSSL may retain only this deterministic, intentionally absent namespace.
if grep -F '/__cup_runtime__/' "$strings_file" |
    grep -Fv '/__cup_runtime__/openssl' >/dev/null; then
    echo "Error: release binary contains an unexpected neutral runtime namespace." >&2
    exit 1
fi
if grep -F -q 'OPENSSLDIR:' "$strings_file" &&
    ! grep -F -q 'OPENSSLDIR: "/__cup_runtime__/openssl"' "$strings_file"; then
    echo "Error: OpenSSL runtime directory is not neutral." >&2
    exit 1
fi
