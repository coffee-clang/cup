#!/bin/sh

# Purpose: Detached POSIX helper that removes the canonical cup root after the parent process exits.
# Inputs: canonical root, copied helper path, parent process id and inherited signal descriptor.
set -u

CUP_ROOT="${1:-}"
SELF_PATH="${2:-}"
PARENT_PID="${3:-}"
PARENT_SIGNAL_FD="${4:-}"
EXPECTED_ROOT="${HOME:-}/.cup"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

# Refuse any request that does not identify this exact helper and canonical root.
[ -n "${HOME:-}" ] || fail "HOME is not set"
case "$HOME" in
    /) fail "HOME must not be the filesystem root" ;;
    /*) ;;
    *) fail "HOME must contain an absolute path" ;;
esac
[ "$CUP_ROOT" = "$EXPECTED_ROOT" ] || fail "refusing to remove a non-canonical cup root"
case "$PARENT_PID" in
    ''|*[!0-9]*)
        fail "invalid parent process id"
        ;;
esac
[ "$PARENT_PID" -gt 0 ] || fail "invalid parent process id"
[ "$PARENT_SIGNAL_FD" = 3 ] || fail "invalid parent lifetime signal"
[ -n "$SELF_PATH" ] && [ "$SELF_PATH" = "$0" ] ||
    fail "self path does not match the running uninstall helper"
[ -f "$SELF_PATH" ] && [ ! -L "$SELF_PATH" ] ||
    fail "the running uninstall helper is not a regular file"
if [ -e "$CUP_ROOT" ] || [ -L "$CUP_ROOT" ]; then
    [ -d "$CUP_ROOT" ] && [ ! -L "$CUP_ROOT" ] ||
        fail "canonical cup root is not a real directory"
fi
UNINSTALL_MARKER="$CUP_ROOT/uninstall.pending"
[ -f "$UNINSTALL_MARKER" ] && [ ! -L "$UNINSTALL_MARKER" ] ||
    fail "uninstall marker is missing or invalid"
marker_line=$(cat "$UNINSTALL_MARKER" 2>/dev/null) ||
    fail "uninstall marker could not be read"
[ "$marker_line" = "parent_pid=$PARENT_PID" ] ||
    fail "uninstall marker does not match the parent process"
[ "$(wc -l < "$UNINSTALL_MARKER" | tr -d '[:space:]')" -eq 1 ] ||
    fail "uninstall marker is invalid"

# Descriptor 3 is inherited only by this helper. Acknowledge the validated handoff, then wait for
# EOF, which proves that the exact parent CUP process released all process-owned handles. The PID
# remains an identity check in the persistent marker.
printf 'R' >&3 || fail "could not acknowledge uninstall handoff"
while IFS= read -r parent_message <&3; do
    :
done
exec 3<&-

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
