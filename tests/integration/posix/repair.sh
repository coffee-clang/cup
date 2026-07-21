#!/bin/sh

# Purpose: Exercises deterministic repair, state reconstruction, quarantine,
# and ambiguous-path preservation.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin repair
prepare_command_environment
run_cup repair >/dev/null

state_file=$TEST_HOME/.cup/state.txt
transaction_file=$TEST_HOME/.cup/transaction.txt

# A valid package found on disk is adopted. Its immutable metadata protection
# is also restored by repair rather than by a test-specific helper.
make_installed_package compiler clang 22.1.5 "$TEST_PLATFORM" clang
output=$(run_cup repair)
assert_contains "$output" "Adopted valid package 'compiler:clang@22.1.5'"
assert_contains "$output" 'Restored read-only protection for clang@22.1.5 metadata.'
assert_contains "$(cat "$state_file")" \
    "installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@22.1.5"
package_metadata="$TEST_HOME/.cup/components/compiler/clang"
package_metadata="$package_metadata/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/info.txt"
package_metadata_mode=$(ls -ld "$package_metadata" | awk '{print $1}')
case "$package_metadata_mode" in
    *w*) fail 'repair did not protect adopted package metadata' ;;
esac

# State entries that no longer have a package are removed together with their
# defaults. This is different from transaction recovery and belongs here.
chmod u+w "$state_file"
cat >> "$state_file" <<STATE
installed.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@22.1.5
default.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@22.1.5
STATE
output=$(run_cup repair)
assert_contains "$output" "Removed stale state record 'debugger:lldb@22.1.5'."
assert_not_contains "$(cat "$state_file")" 'lldb@22.1.5'

# A package-shaped directory with invalid contents is safe to quarantine. A
# malformed path above the package level is reported but deliberately kept.
invalid_package=$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5
mkdir -p "$invalid_package"
mkdir -p "$TEST_HOME/.cup/components/unknown-component"
output=$(run_cup repair)
assert_contains "$output" "Quarantined invalid package '$invalid_package'"
assert_missing "$invalid_package"
find "$TEST_HOME/.cup/recovery" -type d -name package | grep . >/dev/null ||
    fail 'quarantined package was not preserved under recovery'
assert_contains "$output" 'unknown component'
rm -rf "$TEST_HOME/.cup/components/unknown-component"
assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/info.txt"

# Invalid state is preserved and deterministically rebuilt from valid packages
# when no package transaction makes that reconstruction ambiguous.
chmod u+w "$state_file"
printf 'unexpected.key=value\n' > "$state_file"
output=$(run_cup repair)
assert_contains "$output" 'Preserved invalid state as'
assert_file "$state_file.invalid"
assert_contains "$(cat "$state_file")" 'clang@22.1.5'

# Invalid state must not be guessed while a package transaction is pending.
cp "$state_file" "$TMP_ROOT/state.valid"
chmod u+w "$state_file"
printf 'unexpected.key=value\n' > "$state_file"
cat > "$transaction_file" <<JOURNAL
format=1
operation=install
component=compiler
tool=clang
host_platform=$TEST_PLATFORM
target_platform=$TEST_PLATFORM
package_version=22.1.5
temporary_name=install-compiler-clang-$TEST_PLATFORM-$TEST_PLATFORM-22.1.5-test
JOURNAL
run_cup_expect_failure "$TMP_ROOT/ambiguous-state.out" repair
assert_contains "$(cat "$TMP_ROOT/ambiguous-state.out")" \
    'state.txt is missing or invalid while a package transaction is pending'
rm -f "$transaction_file"
cp "$TMP_ROOT/state.valid" "$state_file"

# A malformed journal remains the canonical blocker and ambiguous staging data
# is preserved without any state/package reconciliation.
printf 'not-a-valid-journal\n' > "$transaction_file"
mkdir -p "$TEST_HOME/.cup/staging/ambiguous-data"
state_hash=$(hash_file "$state_file")
run_cup_expect_failure "$TMP_ROOT/invalid-journal.out" repair
output=$(cat "$TMP_ROOT/invalid-journal.out")
assert_contains "$output" 'transaction.txt is invalid'
assert_file "$transaction_file"
assert_equals "$(hash_file "$state_file")" "$state_hash"
[ -d "$TEST_HOME/.cup/staging/ambiguous-data" ] ||
    fail 'repair removed staging data after an invalid journal'
run_cup_expect_failure "$TMP_ROOT/blocked-by-invalid-journal.out" list
assert_contains "$(cat "$TMP_ROOT/blocked-by-invalid-journal.out")" \
    'transaction journal is invalid'
rm -rf "$TEST_HOME/.cup/staging/ambiguous-data" "$transaction_file"

# With no ambiguous journal, stale temporary data is deterministic garbage.
mkdir -p "$TEST_HOME/.cup/staging/stale-data"
run_cup repair >/dev/null
assert_missing "$TEST_HOME/.cup/staging/stale-data"

# Foreign-host state and package trees are diagnostic evidence. Repair must
# preserve them without adopting, quarantining or deleting them.
foreign_host=windows-x64
foreign_tree=$TEST_HOME/.cup/components/compiler/clang/$foreign_host/$foreign_host/22.1.5
mkdir -p "$foreign_tree"
chmod u+w "$state_file"
printf 'installed.compiler.%s.%s=clang@22.1.5\n' \
    "$foreign_host" "$foreign_host" >> "$state_file"
run_cup_expect_failure "$TMP_ROOT/foreign-doctor.out" doctor
assert_contains "$(cat "$TMP_ROOT/foreign-doctor.out")" \
    'record(s) for foreign hosts'
assert_contains "$(cat "$TMP_ROOT/foreign-doctor.out")" \
    'foreign-host package tree(s)'
output=$(run_cup repair)
assert_contains "$output" 'Preserved 1 foreign-host package tree(s)'
assert_contains "$(cat "$state_file")" \
    "installed.compiler.$foreign_host.$foreign_host=clang@22.1.5"
[ -d "$foreign_tree" ] || fail 'repair removed a foreign-host package tree'
run_cup_expect_failure "$TMP_ROOT/foreign-operational.out" list
assert_contains "$(cat "$TMP_ROOT/foreign-operational.out")" \
    'foreign host'
chmod u+w "$state_file"
grep -v "installed.compiler.$foreign_host.$foreign_host=" "$state_file" \
    > "$state_file.tmp"
mv "$state_file.tmp" "$state_file"
rm -rf "$TEST_HOME/.cup/components/compiler/clang/$foreign_host"

# Repair cannot race a detached uninstall operation.
: > "$TEST_HOME/.cup/uninstall.pending"
run_cup_expect_failure "$TMP_ROOT/pending-uninstall.out" repair
assert_contains "$(cat "$TMP_ROOT/pending-uninstall.out")" \
    'cup uninstall is in progress or did not finish'
rm -f "$TEST_HOME/.cup/uninstall.pending"
assert_cup_healthy

printf 'Repair integration tests passed for %s.\n' "$TEST_PLATFORM"
