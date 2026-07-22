#!/bin/sh

# Purpose: Detached POSIX helper that removes the canonical cup root after the parent process exits.
# Inputs: canonical root, copied helper path and parent process id.
set -u

CUP_ROOT="${1:-}"
SELF_PATH="${2:-}"
PARENT_PID="${3:-}"
EXPECTED_ROOT="${HOME:-}/.cup"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

# Refuse any request that does not identify this exact helper and canonical root.
[ -n "${HOME:-}" ] || fail "HOME is not set"
[ "$CUP_ROOT" = "$EXPECTED_ROOT" ] || fail "refusing to remove a non-canonical cup root"
case "$PARENT_PID" in
    ''|*[!0-9]*)
        fail "invalid parent process id"
        ;;
esac
[ "$PARENT_PID" -gt 0 ] || fail "invalid parent process id"
[ -n "$SELF_PATH" ] && [ "$SELF_PATH" = "$0" ] ||
    fail "self path does not match the running uninstall helper"

# The installed process must release its executable and root before detachment.
while kill -0 "$PARENT_PID" 2>/dev/null; do
    sleep 1
done

# Rename is the uninstall commit point; recursive deletion happens afterward.
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

rm -f -- "$SELF_PATH" || exit 1

exit 0
