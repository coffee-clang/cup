#!/usr/bin/env sh

# Purpose: Executes unit-test binaries previously compiled by the Makefile.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM=$CUP_TEST_PLATFORM
CONFIGURATION=${CUP_TEST_CONFIGURATION:-development}
TEST_BUILD_DIR="$ROOT/build/$PLATFORM/$CONFIGURATION/tests/unit"

[ -d "$TEST_BUILD_DIR" ] || {
    printf 'Unit tests are not built: %s\n' "$TEST_BUILD_DIR" >&2
    printf 'Run make PLATFORM=%s test-unit-build first.\n' "$PLATFORM" >&2
    exit 1
}

found=0
for test_binary in "$TEST_BUILD_DIR"/test_*; do
    [ -f "$test_binary" ] || continue
    case "$test_binary" in
        *.gcda|*.gcno) continue ;;
    esac
    [ -x "$test_binary" ] || {
        printf 'Unit-test binary is not executable: %s\n' "$test_binary" >&2
        exit 1
    }
    found=1
    printf '==> Running C unit test: %s\n' "$(basename "$test_binary")"
    "$test_binary"
done
[ "$found" -eq 1 ] || {
    printf 'No unit-test binaries found in %s\n' "$TEST_BUILD_DIR" >&2
    exit 1
}
printf 'All C unit tests passed for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
