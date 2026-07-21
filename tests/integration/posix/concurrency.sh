#!/bin/sh

# Purpose: Exercises process locking and verifies that concurrent mutation
# leaves one coherent installation.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin concurrency
prepare_command_environment
run_cup repair >/dev/null
make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++

(
    run_cup install compiler clang@stable > "$TMP_ROOT/install-a.out" 2>&1
) &
pid_a=$!
(
    run_cup install compiler clang@stable > "$TMP_ROOT/install-b.out" 2>&1
) &
pid_b=$!

status_a=0
status_b=0
wait "$pid_a" || status_a=$?
wait "$pid_b" || status_b=$?

if { [ "$status_a" -eq 0 ] && [ "$status_b" -eq 0 ]; } ||
    { [ "$status_a" -ne 0 ] && [ "$status_b" -ne 0 ]; }; then
    printf 'install A status/output: %s\n%s\n' \
        "$status_a" "$(cat "$TMP_ROOT/install-a.out")" >&2
    printf 'install B status/output: %s\n%s\n' \
        "$status_b" "$(cat "$TMP_ROOT/install-b.out")" >&2
    fail 'expected exactly one concurrent install to succeed'
fi

failed_output=$TMP_ROOT/install-a.out
[ "$status_a" -ne 0 ] || failed_output=$TMP_ROOT/install-b.out
failed_text=$(cat "$failed_output")
case "$failed_text" in
    *'already installed'* | \
        *'another cup operation is currently running'* | \
        *'interrupted package transaction must be repaired first'*) ;;
    *) fail 'concurrent loser did not report a lock or installation conflict' ;;
esac

assert_cup_healthy
assert_missing "$TEST_HOME/.cup/transaction.txt"
if find "$TEST_HOME/.cup/staging" -mindepth 1 -print -quit | grep . >/dev/null; then
    find "$TEST_HOME/.cup/staging" -mindepth 1 -maxdepth 1 -print >&2
    fail 'concurrent installs left temporary paths behind'
fi
assert_contains "$(run_cup info compiler)" "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"
assert_equals "$(run_native_wrapper clang)" "clang-22.1.5-$TEST_PLATFORM:clang"

printf 'Concurrency integration tests passed for %s.\n' "$TEST_PLATFORM"
