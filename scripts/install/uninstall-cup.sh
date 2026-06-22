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
    STAGING_ROOT=$(mktemp -d "$HOME/.cup-uninstall.XXXXXX") ||
        fail "could not create uninstall staging directory"
    STAGED_CUP_ROOT="$STAGING_ROOT/root"

    if ! mv -- "$CUP_ROOT" "$STAGED_CUP_ROOT"; then
        rmdir -- "$STAGING_ROOT" 2>/dev/null || true
        fail "could not detach $CUP_ROOT"
    fi

    rm -rf -- "$STAGING_ROOT" ||
        fail "could not remove detached cup installation"
fi

if [ -n "$SELF_PATH" ]; then
    rm -f -- "$SELF_PATH" || exit 1
fi

exit 0
