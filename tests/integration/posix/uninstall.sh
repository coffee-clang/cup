#!/bin/sh

# Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"
. "$TESTS_ROOT/support/posix/uninstall.sh"

uninstall_started_message='Uninstall handoff accepted; cleanup continues in the background. '
uninstall_started_message="${uninstall_started_message}You can close this terminal. "
uninstall_started_message="${uninstall_started_message}The PATH entry was not removed."

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture\n' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" "$uninstall_started_message"
assert_contains "$output" 'Recovery path if cleanup fails: '
assert_contains "$output" '.cup-uninstall-'

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi

printf 'Uninstall integration tests passed for %s.\n' "$TEST_PLATFORM"
