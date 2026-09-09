#!/bin/sh

# Exercises read-only diagnosis and verifies doctor never repairs observed damage.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin doctor
prepare_command_environment

# Doctor is read-only and must also handle a repository development checkout
# before the runtime has ever been initialized.
output=$(run_cup doctor)
assert_contains "$output" 'development cup assets are available'
assert_contains "$output" 'cup runtime is not initialized'
assert_contains "$output" 'Doctor found no issues.'

run_cup repair >/dev/null
run_cup doctor > "$TMP_ROOT/doctor-path.out"
assert_contains "$(cat "$TMP_ROOT/doctor-path.out")" \
    'current CUP command directory is not in PATH'
assert_contains "$(cat "$TMP_ROOT/doctor-path.out")" \
    "export PATH='$TEST_HOME/.cup/bin':\"\$PATH\""

# A valid root whose bin path contains the PATH separator cannot be represented as one entry.
(
    TEST_HOME="$TMP_ROOT/home:separator"
    export TEST_HOME
    mkdir -p "$TEST_HOME"
    run_cup repair >/dev/null
    separator_output=$(run_cup doctor)
    assert_contains "$separator_output" 'current CUP command directory is not in PATH'
    assert_contains "$separator_output" \
        "contains ':' and cannot be represented as one PATH entry"
    assert_not_contains "$separator_output" 'export PATH='
)

# Shell-significant characters that are valid inside one PATH entry are quoted exactly.
(
    TEST_HOME="$TMP_ROOT/home'quote"
    export TEST_HOME
    mkdir -p "$TEST_HOME"
    run_cup repair >/dev/null
    quote_output=$(run_cup doctor)
    assert_contains "$quote_output" \
        "export PATH='$TMP_ROOT/home'\"'\"'quote/.cup/bin':\"\$PATH\""
)

PATH="$TEST_HOME/.cup/bin:$PATH"
export PATH
state_file=$TEST_HOME/.cup/state.txt

# Create independent diagnostic conditions. The package is deliberately not in
# state, another state record has no package, one installed package is changed
# after its manifest was generated and is absent from the current catalog, and
# runtime leftovers are present.
make_installed_package compiler clang 99.0.0 "$TEST_PLATFORM" clang
make_installed_package debugger lldb 23.1.0 "$TEST_PLATFORM" lldb
compiler_info=$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/99.0.0/info.txt
printf 'fixture.note=changed-after-manifest\n' >> "$compiler_info"
compiler_info_hash=$(hash_file "$compiler_info")
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
state_hash=$(hash_file "$state_file")

run_cup_expect_failure "$TMP_ROOT/doctor-issues.out" doctor
output=$(cat "$TMP_ROOT/doctor-issues.out")
assert_contains "$output" 'transaction journal is invalid'
assert_contains "$output" "installed state record 'linter:clang-tidy@22.1.5' has no valid package"
assert_contains "$output" "installed package 'compiler:clang@99.0.0' is not listed"
assert_contains "$output" \
    "package path '$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/99.0.0' is invalid"
assert_contains "$output" \
    "valid package 'lldb@23.1.0' exists in components but is absent from state.txt"
assert_contains "$output" "package path '$invalid_package' is invalid"
assert_contains "$output" 'staging directory contains 1 leftover item(s)'
assert_contains "$output" 'Run '\''cup repair'\'' after reviewing them.'

# The diagnostic command must not repair or remove any of those conditions.
assert_equals "$(hash_file "$state_file")" "$state_hash"
[ -d "$invalid_package" ] || fail 'doctor modified invalid package path'
[ -d "$TEST_HOME/.cup/staging/leftover" ] || fail 'doctor removed staging data'
assert_equals "$(hash_file "$compiler_info")" "$compiler_info_hash"

rm -f "$TEST_HOME/.cup/transaction.txt"
rm -rf "$TEST_HOME/.cup/staging/leftover" "$invalid_package"
make_installed_package compiler clang 99.0.0 "$TEST_PLATFORM" clang
chmod u+w "$state_file"
cat > "$state_file" <<STATE
format=1
installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@99.0.0
installed.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@23.1.0
STATE
# An incomplete runtime is reported, and a missing lock prevents an unsafe
# snapshot from being treated as coherent.
rm -rf "$TEST_HOME/.cup/cache"
run_cup_expect_failure "$TMP_ROOT/doctor-incomplete.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-incomplete.out")" \
    'missing cache directory'
mkdir -p "$TEST_HOME/.cup/cache"
rm -f "$TEST_HOME/.cup/cup.lock"
run_cup_expect_failure "$TMP_ROOT/doctor-missing-lock.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-missing-lock.out")" \
    'cup lock file is missing'

run_cup repair >/dev/null
output=$(run_cup doctor)
assert_contains "$output" 'Doctor found 1 warning(s), but no blocking issues.'
assert_contains "$output" "installed package 'compiler:clang@99.0.0' is not listed"

cat > "$TEST_HOME/.cup/transaction.txt" <<'JOURNAL'
format=1
operation=cup-update
phase=failed
temporary_name=cup-update-test
token=fixture-cup-update-test
version=0.3.0
error=15
recovery=pending
JOURNAL
update_journal_hash=$(hash_file "$TEST_HOME/.cup/transaction.txt")
run_cup_expect_failure "$TMP_ROOT/doctor-update-failed.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-update-failed.out")" \
    'the previous cup update to version 0.3.0 failed with error 15; recovery is pending'
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$update_journal_hash"

# Help, version, typos, parse errors and read-only views never rewrite durable evidence.
run_cup help >/dev/null
run_cup --version >/dev/null
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$update_journal_hash"
run_cup_expect_failure "$TMP_ROOT/doctor-typo.out" not-a-command
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$update_journal_hash"
run_cup_expect_failure "$TMP_ROOT/doctor-parse.out" install
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$update_journal_hash"
for read_only_command in search list config info inspect; do
    run_cup_expect_failure "$TMP_ROOT/doctor-$read_only_command.out" "$read_only_command"
    assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$update_journal_hash"
done

cat > "$TEST_HOME/.cup/transaction.txt" <<'JOURNAL'
format=1
operation=cup-update
phase=failed
temporary_name=cup-update-test
token=fixture-cup-update-test
version=NEWER
error=15
recovery=pending
JOURNAL
run_cup_expect_failure "$TMP_ROOT/doctor-update-invalid.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-update-invalid.out")" \
    'cup update journal is invalid'
rm -f "$TEST_HOME/.cup/transaction.txt"

cat > "$TEST_HOME/.cup/transaction.txt" <<'JOURNAL'
format=2
operation=uninstall
phase=failed
temporary_name=.cup-uninstall-fixture
token=fixture
error=6
JOURNAL
uninstall_journal_hash=$(hash_file "$TEST_HOME/.cup/transaction.txt")
run_cup_expect_failure "$TMP_ROOT/doctor-uninstall-failed.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-uninstall-failed.out")" \
    "the previous cup uninstall failed with error 6"
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$uninstall_journal_hash"
run_cup_expect_failure "$TMP_ROOT/doctor-uninstall-failed-again.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-uninstall-failed-again.out")" \
    "the previous cup uninstall failed with error 6"
assert_equals "$(hash_file "$TEST_HOME/.cup/transaction.txt")" "$uninstall_journal_hash"
rm -f "$TEST_HOME/.cup/transaction.txt"

printf 'Doctor integration tests passed for %s.\n' "$TEST_PLATFORM"
