#!/bin/sh

# Purpose: Exercises read-only diagnosis and verifies doctor never repairs observed damage.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin doctor
prepare_command_environment

# Doctor is read-only and must also handle a repository development checkout
# before the runtime has ever been initialized.
output=$(run_cup doctor)
assert_contains "$output" 'development CUP assets are available'
assert_contains "$output" 'cup runtime is not initialized'
assert_contains "$output" 'Doctor found no issues.'

run_cup repair >/dev/null
state_file=$TEST_HOME/.cup/state.txt

# Create independent diagnostic conditions. The package is deliberately not in
# state, another state record has no package, one valid package is unprotected
# and absent from the active catalog, and runtime leftovers are present.
make_installed_package compiler clang 99.0.0 "$TEST_PLATFORM" clang
make_installed_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb
invalid_package=$TEST_HOME/.cup/components/linker/lld/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5
mkdir -p "$invalid_package"
chmod u+w "$state_file"
cat > "$state_file" <<STATE
format=1
installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@99.0.0
installed.linter.$TEST_PLATFORM.$TEST_PLATFORM=clang-tidy@22.1.5
STATE
mkdir -p "$TEST_HOME/.cup/staging/leftover"
printf 'invalid journal\n' > "$TEST_HOME/.cup/transaction.txt"
: > "$TEST_HOME/.cup/uninstall.pending"
state_hash=$(hash_file "$state_file")

run_cup_expect_failure "$TMP_ROOT/doctor-issues.out" doctor
output=$(cat "$TMP_ROOT/doctor-issues.out")
assert_contains "$output" 'an uninstall marker exists'
assert_contains "$output" 'transaction journal is invalid'
assert_contains "$output" "installed state record 'linter:clang-tidy@22.1.5' has no valid package"
assert_contains "$output" "package metadata for 'compiler:clang@99.0.0' is not read-only"
assert_contains "$output" "installed package 'compiler:clang@99.0.0' is not listed"
assert_contains "$output" "valid package 'lldb@22.1.5' exists in components but is absent from state.txt"
assert_contains "$output" "package path '$invalid_package' is invalid"
assert_contains "$output" 'staging directory contains 1 leftover item(s)'
assert_contains "$output" 'Run '\''cup repair'\'' after reviewing them.'

# The diagnostic command must not repair or remove any of those conditions.
assert_equals "$(hash_file "$state_file")" "$state_hash"
[ -d "$invalid_package" ] || fail 'doctor modified invalid package path'
[ -d "$TEST_HOME/.cup/staging/leftover" ] || fail 'doctor removed staging data'
assert_file "$TEST_HOME/.cup/uninstall.pending"
package_metadata="$TEST_HOME/.cup/components/compiler/clang"
package_metadata="$package_metadata/$TEST_PLATFORM/$TEST_PLATFORM/99.0.0/info.txt"
package_metadata_mode=$(ls -ld "$package_metadata" | awk '{print $1}')
case "$package_metadata_mode" in
    *w*) ;;
    *) fail 'doctor changed package metadata permissions' ;;
esac

rm -f "$TEST_HOME/.cup/uninstall.pending" "$TEST_HOME/.cup/transaction.txt"
rm -rf "$TEST_HOME/.cup/staging/leftover" "$invalid_package"
chmod u+w "$state_file"
cat > "$state_file" <<STATE
format=1
installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@99.0.0
installed.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@22.1.5
STATE
chmod 0444 "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/99.0.0/info.txt"
chmod 0444 "$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/info.txt"

# An incomplete runtime is reported, and a missing lock prevents an unsafe
# snapshot from being treated as coherent.
rm -rf "$TEST_HOME/.cup/cache"
run_cup_expect_failure "$TMP_ROOT/doctor-incomplete.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-incomplete.out")" \
    'cup runtime structure is incomplete'
mkdir -p "$TEST_HOME/.cup/cache"
rm -f "$TEST_HOME/.cup/cup.lock"
run_cup_expect_failure "$TMP_ROOT/doctor-missing-lock.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-missing-lock.out")" \
    'cup lock file is missing'

run_cup repair >/dev/null
output=$(run_cup doctor)
assert_contains "$output" 'Doctor found 1 warning(s), but no blocking issues.'
assert_contains "$output" "installed package 'compiler:clang@99.0.0' is not listed"

cat > "$TEST_HOME/.cup/cup-update-result.txt" <<'RESULT'
format=1
status=failed
error=15
version=0.3.0
RESULT
run_cup_expect_failure "$TMP_ROOT/doctor-update-failed.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-update-failed.out")" \
    'the previous CUP update failed with error 15 at version 0.3.0'

printf 'invalid update result\n' > "$TEST_HOME/.cup/cup-update-result.txt"
run_cup_expect_failure "$TMP_ROOT/doctor-update-invalid.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-update-invalid.out")" \
    'the previous CUP update result is invalid'
rm -f "$TEST_HOME/.cup/cup-update-result.txt"

printf 'Doctor integration tests passed for %s.\n' "$TEST_PLATFORM"
