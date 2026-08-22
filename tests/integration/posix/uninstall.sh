#!/bin/sh

# Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"
. "$TESTS_ROOT/support/posix/uninstall.sh"

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture\n' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi


# A failed cleanup must keep enough ownership evidence to identify the detached residue without
# guessing at unrelated data. The mock fails only when the helper reaches the components directory.
prepare_command_environment
run_cup repair >/dev/null
cup_root=$TEST_HOME/.cup
mkdir -p "$cup_root/components" "$cup_root/bin"
printf 'fixture\n' > "$cup_root/components/fixture.txt"
cp "$CUP" "$cup_root/bin/cup"
chmod +x "$cup_root/bin/cup"

# A native cleanup failure must preserve transaction.txt while managed residue remains. Make a
# managed directory unreadable so the detached helper cannot traverse it; no shell-command mock is
# involved in the native cleanup path.
blocked_components="$cup_root/components"
chmod 000 "$blocked_components"

output=$(run_cup uninstall --yes)
assert_contains "$output" 'Uninstall started. The PATH entry was not removed.'

residue=
attempt=0
while [ "$attempt" -lt 200 ]; do
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    if [ -n "$residue" ] && [ -f "$residue/transaction.txt" ] &&
        grep -F 'phase=detaching' "$residue/transaction.txt" >/dev/null 2>&1 &&
        grep -F 'stage=detach' "$residue/transaction.txt" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -n "$residue" ] || fail 'failed uninstall did not leave a detached residue'
assert_missing "$cup_root"
assert_file "$residue/transaction.txt"
[ -d "$residue/components" ] || fail 'failed uninstall removed the blocked managed residue'
chmod 700 "$residue/components"

printf 'Uninstall integration tests passed for %s.\n' "$TEST_PLATFORM"
