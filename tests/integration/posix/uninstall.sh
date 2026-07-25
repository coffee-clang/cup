#!/bin/sh

# Purpose: Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture
' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

attempt=0
while [ -e "$cup_root" ] && [ "$attempt" -lt 200 ]; do
    sleep 0.1
    attempt=$((attempt + 1))
done
assert_missing "$cup_root"
if find "$TEST_HOME" -name '.cup-uninstall.*' -print | grep . >/dev/null 2>&1; then
    fail 'uninstall helper left its staging directory behind'
fi

printf 'Uninstall integration tests passed for %s.
' "$TEST_PLATFORM"
