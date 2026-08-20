#!/bin/sh

# Runs every native POSIX integration suite in a stable order.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment

run_suite() {
    script=$1
    name=${script##*/}
    name=${name%.sh}
    label=$(printf '%s' "$name" | tr '-' ' ')

    printf '==> Testing %s...\n' "$label"
    if [ -n "${CUP_TEST_SUITE_TIMEOUT:-}" ]; then
        case "$CUP_TEST_SUITE_TIMEOUT" in
            *[!0-9]*|''|0)
                printf 'Invalid CUP_TEST_SUITE_TIMEOUT: %s\n' \
                    "$CUP_TEST_SUITE_TIMEOUT" >&2
                exit 2
                ;;
        esac
        timeout_command=$(cup_test_find_timeout) || exit 2
        "$timeout_command" --foreground --signal=TERM --kill-after=30s \
            "$CUP_TEST_SUITE_TIMEOUT" "$script"
    else
        "$script"
    fi
}

TESTS_ROOT="$ROOT/tests"
export TESTS_ROOT
. "$TESTS_ROOT/support/posix/cli.sh"
require_test_binary

found=0
for script in "$ROOT"/tests/integration/posix/*.sh; do
    [ -f "$script" ] || continue
    found=1
    run_suite "$script"
done
[ "$found" -eq 1 ] || {
    printf 'No POSIX integration suites were found.\n' >&2
    exit 2
}

printf 'All POSIX integration tests passed.\n'
