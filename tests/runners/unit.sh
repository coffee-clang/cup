#!/usr/bin/env sh

# Executes unit-test binaries previously compiled by the Makefile.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM=$CUP_TEST_PLATFORM
CONFIGURATION=${CUP_TEST_CONFIGURATION:-development}
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
TEST_BUILD_DIR="$TEST_BUILD_ROOT/$PLATFORM/$CONFIGURATION/tests/unit"

[ -d "$TEST_BUILD_DIR" ] || {
    printf 'Unit tests are not built: %s\n' "$TEST_BUILD_DIR" >&2
    printf 'Run make PLATFORM=%s test-unit-build first.\n' "$PLATFORM" >&2
    exit 1
}

UNIT_TIMEOUT=${CUP_TEST_UNIT_TIMEOUT:-}
TIMEOUT_COMMAND=
if [ -n "$UNIT_TIMEOUT" ]; then
    case "$UNIT_TIMEOUT" in
        *[!0-9]*|0)
            printf 'Invalid CUP_TEST_UNIT_TIMEOUT: %s\n' "$UNIT_TIMEOUT" >&2
            exit 2
            ;;
    esac
    TIMEOUT_COMMAND=$(cup_test_find_timeout) || exit 2
fi

expected_list=$(mktemp "${TMPDIR:-/tmp}/cup-unit-expected.XXXXXX") || exit 1
actual_list=$(mktemp "${TMPDIR:-/tmp}/cup-unit-actual.XXXXXX") || {
    rm -f -- "$expected_list"
    exit 1
}
cleanup_lists() { rm -f -- "$expected_list" "$actual_list"; }
trap cleanup_lists EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"$ROOT/tests/build/unit.sh" --list "$PLATFORM" | LC_ALL=C sort > "$expected_list"
: > "$actual_list"
for test_binary in "$TEST_BUILD_DIR"/test_*; do
    [ -f "$test_binary" ] || continue
    case "$test_binary" in
        *.gcda|*.gcno) continue ;;
    esac
    [ -x "$test_binary" ] || {
        printf 'Unit-test binary is not executable: %s\n' "$test_binary" >&2
        exit 1
    }
    basename "$test_binary" >> "$actual_list"
done
LC_ALL=C sort -o "$actual_list" "$actual_list"
if [ "$(cat "$expected_list")" != "$(cat "$actual_list")" ]; then
    printf 'Expected unit-test binaries:\n' >&2
    cat "$expected_list" >&2
    printf 'Available unit-test binaries:\n' >&2
    cat "$actual_list" >&2
    exit 1
fi

while IFS= read -r test_name; do
    [ -n "$test_name" ] || continue
    test_binary="$TEST_BUILD_DIR/$test_name"
    printf '==> Running C unit test: %s\n' "$test_name"
    if [ -n "$TIMEOUT_COMMAND" ]; then
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s \
            "$UNIT_TIMEOUT" "$test_binary"
    else
        "$test_binary"
    fi
done < "$actual_list"
trap - EXIT HUP INT TERM
cleanup_lists
printf 'All C unit tests passed for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
