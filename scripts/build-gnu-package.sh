#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<USAGE
Usage:
  $0 <gcc|gdb> <version|stable|latest> <host_platform> <target_platform> <revision>

Compatibility mode:
  $0 <gcc|gdb> <version|stable|latest> <legacy_build_mode>

Examples:
  $0 gcc stable linux-x64 linux-x64 1
  $0 gcc stable linux-x64 windows-x64 1
  $0 gdb stable windows-x64 windows-x64 1
USAGE
}

if [ "$#" -eq 5 ]; then
    TOOL="$1"
    VERSION="$2"
    HOST_PLATFORM="$3"
    TARGET_PLATFORM="$4"
    REVISION="$5"
elif [ "$#" -eq 3 ]; then
    TOOL="$1"
    VERSION="$2"
    HOST_PLATFORM="linux-x64"
    TARGET_PLATFORM="linux-x64"
    REVISION="1"
else
    usage >&2
    exit 2
fi

case "$TOOL" in
    gcc)
        exec "$SCRIPT_DIR/build-gcc.sh" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION"
        ;;
    gdb)
        exec "$SCRIPT_DIR/build-gdb.sh" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION"
        ;;
    *)
        printf 'Unsupported GNU tool: %s\n' "$TOOL" >&2
        exit 2
        ;;
esac
