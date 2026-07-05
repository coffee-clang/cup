#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$ROOT/scripts/tests/unit.sh"
"$ROOT/scripts/tests/integration.sh"
