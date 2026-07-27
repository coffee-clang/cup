#!/bin/sh

# Purpose: Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"
. "$TESTS_ROOT/support/uninstall.sh"

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture
' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi

printf 'Uninstall integration tests passed for %s.
' "$TEST_PLATFORM"
