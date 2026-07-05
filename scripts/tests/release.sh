#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: scripts/tests/release.sh <release-directory>" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/scripts/tests/release/posix.sh" "$1"
