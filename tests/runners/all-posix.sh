#!/bin/sh

# Purpose: Composes the complete normal POSIX source regression sequence.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$ROOT/tests/runners/unit.sh"
"$ROOT/tests/runners/integration-posix.sh"
CUP_TEST_WITH_BUILD_OUTPUT=1 "$ROOT/tests/runners/repository.sh"
