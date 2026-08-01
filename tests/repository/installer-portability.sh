#!/usr/bin/env sh

# Purpose: Verifies that the public POSIX scripts parse with supported shell implementations.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
INSTALLER="$ROOT/scripts/install/install-cup.sh"
UNINSTALLER="$ROOT/scripts/install/uninstall-cup.sh"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

for script in "$INSTALLER" "$UNINSTALLER"; do
    sh -n "$script" || fail "default sh rejected $script"
    if command -v dash >/dev/null 2>&1; then
        dash -n "$script" || fail "dash rejected $script"
    fi
    if command -v busybox >/dev/null 2>&1; then
        busybox sh -n "$script" || fail "BusyBox sh rejected $script"
    fi
done

printf 'Installer shell compatibility checks passed.\n'
