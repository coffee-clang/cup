#!/usr/bin/env bash

# Purpose: Builds with ASan/UBSan and reuses C and POSIX integration owners.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"

if [ "$(uname -s)" != Linux ] || [ "$PLATFORM" != linux-x64 ]; then
    printf 'Sanitizer tests are supported only on linux-x64.\n' >&2
    exit 2
fi

cup_test_require_dependencies

export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:detect_leaks=1:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
export CUP_TEST_SKIP_BUILD=1
export CUP_TEST_BINARY="$ROOT/build/$PLATFORM/sanitizers/bin/cup"

make -C "$ROOT" clean >/dev/null
make -C "$ROOT" PLATFORM="$PLATFORM" sanitizers -j2 >/dev/null
make -C "$ROOT" PLATFORM="$PLATFORM" CC=gcc \
    CUP_TEST_CONFIGURATION=sanitizers test-unit-build test-helpers >/dev/null

CUP_TEST_CONFIGURATION=sanitizers "$ROOT/tests/runners/unit.sh"
CUP_TEST_CONFIGURATION=sanitizers "$ROOT/tests/runners/integration-posix.sh"
printf 'Sanitizer tests passed for %s.\n' "$PLATFORM"
