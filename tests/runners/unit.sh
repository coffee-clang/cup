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
GCOV_PREFIX_VALUE=
GCOV_PREFIX_STRIP_VALUE=
if [ "$PLATFORM:$CONFIGURATION" = windows-x64:coverage ]; then
    . "$ROOT/tests/support/posix/coverage.sh"
    command -v cygpath >/dev/null 2>&1 || {
        printf 'cygpath is required for Windows GCC coverage relocation.\n' >&2
        exit 2
    }
    GCOV_PREFIX_VALUE=$(cygpath -m "$TEST_BUILD_DIR") || exit 1
    GCOV_PREFIX_STRIP_VALUE=$(
        cup_coverage_gcov_strip_components "$GCOV_PREFIX_VALUE") || exit 1
fi

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
    test_name=${test_binary##*/}
    printf '==> Running C unit test: %s\n' "$test_name"
    if [ -n "$GCOV_PREFIX_VALUE" ]; then
        if [ -n "$TIMEOUT_COMMAND" ]; then
            env GCOV_PREFIX="$GCOV_PREFIX_VALUE" \
                GCOV_PREFIX_STRIP="$GCOV_PREFIX_STRIP_VALUE" \
                "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s \
                "$UNIT_TIMEOUT" "$test_binary"
        else
            env GCOV_PREFIX="$GCOV_PREFIX_VALUE" \
                GCOV_PREFIX_STRIP="$GCOV_PREFIX_STRIP_VALUE" "$test_binary"
        fi
    elif [ -n "$TIMEOUT_COMMAND" ]; then
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s \
            "$UNIT_TIMEOUT" "$test_binary"
    else
        "$test_binary"
    fi
done
[ "$found" -eq 1 ] || {
    printf 'No C unit-test binaries were found in %s.\n' "$TEST_BUILD_DIR" >&2
    exit 1
}
printf 'All C unit tests passed for %s (%s).\n' "$PLATFORM" "$CONFIGURATION"
