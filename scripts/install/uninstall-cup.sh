#!/bin/sh
set -u

CUP_ROOT="${1:-}"
SELF_PATH="${2:-}"
PARENT_PID="${3:-}"
EXPECTED_ROOT="${HOME:-}/.cup"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

[ -n "${HOME:-}" ] || fail "HOME is not set"
[ "$CUP_ROOT" = "$EXPECTED_ROOT" ] || fail "refusing to remove a non-canonical cup root"
case "$PARENT_PID" in
    ''|*[!0-9]*) fail "invalid parent process id" ;;
esac

while kill -0 "$PARENT_PID" 2>/dev/null; do
    sleep 1
done

if [ -e "$CUP_ROOT" ] || [ -L "$CUP_ROOT" ]; then
    rm -rf -- "$CUP_ROOT" || fail "could not remove $CUP_ROOT"
fi

if [ -n "$SELF_PATH" ]; then
    rm -f -- "$SELF_PATH" || exit 1
fi

exit 0
