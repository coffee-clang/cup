#!/usr/bin/env sh

# Rejects machine-specific and transactional paths in release binaries.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$ROOT_DIR/scripts/lib/path-safety.sh"

binary=${1:?binary is required}
shift

case "$binary" in /*|[A-Za-z]:/*) ;; *) binary=$(pwd -P)/$binary ;; esac
cup_path_require_regular_file "$binary" 'release binary' || exit 2
[ -s "$binary" ] || {
    echo "Error: release binary must be non-empty: $binary" >&2
    exit 2
}

if ! command -v strings >/dev/null 2>&1; then
    echo "Error: strings is required to inspect release binaries." >&2
    exit 2
fi

strings_file=$(mktemp "${TMPDIR:-/tmp}/cup-release-strings.XXXXXX")
cleanup_strings() { rm -f -- "$strings_file"; }
trap cleanup_strings EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
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
