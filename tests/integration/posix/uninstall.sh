#!/bin/sh

# Exercises the public detached uninstall workflow and canonical-root cleanup.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"
. "$TESTS_ROOT/support/posix/uninstall.sh"

uninstall_started_message='Uninstall started; cleanup continues in the background. '
uninstall_started_message="${uninstall_started_message}You can close this terminal. "
uninstall_started_message="${uninstall_started_message}The PATH entry was not removed."

test_begin uninstall
prepare_command_environment
run_cup repair >/dev/null

cup_root=$TEST_HOME/.cup
printf 'fixture\n' > "$cup_root/components/fixture.txt"
output=$(run_cup uninstall --yes)
assert_contains "$output" "$uninstall_started_message"

if ! cup_test_wait_for_uninstall "$cup_root" "$TEST_HOME"; then
    residue=$(cup_test_uninstall_residue "$TEST_HOME")
    assert_missing "$cup_root"
    fail "uninstall helper left staging behind: $residue"
fi


# A failed cleanup must keep enough ownership evidence to identify the detached residue without
# guessing at unrelated data.
prepare_command_environment
run_cup repair >/dev/null
cup_root=$TEST_HOME/.cup
mkdir -p "$cup_root/components" "$cup_root/bin"
printf 'fixture\n' > "$cup_root/components/fixture.txt"
cp "$CUP" "$cup_root/bin/cup"
chmod +x "$cup_root/bin/cup"

# A native cleanup failure must preserve transaction.txt while managed residue remains. Exceed the
# bounded native tree depth so the failure is deterministic even for privileged test users; no
# shell-command mock is involved in the native cleanup path.
blocked_components="$cup_root/components"
deep="$blocked_components"
depth=0
while [ "$depth" -le 130 ]; do
    deep="$deep/d"
    mkdir "$deep"
    depth=$((depth + 1))
done

output=$(run_cup uninstall --yes)
assert_contains "$output" "$uninstall_started_message"

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
[ -d "$residue/components" ] || fail 'failed uninstall removed the bounded-depth residue'

printf 'Uninstall integration tests passed for %s.\n' "$TEST_PLATFORM"
