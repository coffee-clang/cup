#!/bin/sh

# Purpose: Exercises state persistence, duplicate/invalid records and state-to-package consistency.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin state
prepare_command_environment
package_catalog_add_version compiler clang "$TEST_PLATFORM" 21.1.5
run_cup repair >/dev/null

make_package compiler clang 21.1.5 "$TEST_PLATFORM" clang
make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang
make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb
make_package linker lld 22.1.5 "$TEST_PLATFORM" lld

run_cup install compiler clang@21.1.5 >/dev/null
run_cup install compiler clang@stable >/dev/null
run_cup install debugger lldb@stable >/dev/null
run_cup install linker lld@stable >/dev/null

state_file=$TEST_HOME/.cup/state.txt
assert_file "$state_file"
assert_equals "$(sed -n '1p' "$state_file")" 'format=1'
installed_count=$(grep -c '^installed\.' "$state_file")
active_count=$(grep -c '^default\.' "$state_file")
assert_equals "$installed_count" '4'
assert_equals "$active_count" '3'

state_text=$(cat "$state_file")
assert_contains "$state_text" \
    "installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@21.1.5"
assert_contains "$state_text" \
    "installed.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@22.1.5"
assert_contains "$state_text" \
    "installed.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@22.1.5"
assert_contains "$state_text" \
    "installed.linker.$TEST_PLATFORM.$TEST_PLATFORM=lld@22.1.5"
assert_contains "$state_text" \
    "default.compiler.$TEST_PLATFORM.$TEST_PLATFORM=clang@21.1.5"
assert_contains "$state_text" \
    "default.debugger.$TEST_PLATFORM.$TEST_PLATFORM=lldb@22.1.5"
assert_contains "$state_text" \
    "default.linker.$TEST_PLATFORM.$TEST_PLATFORM=lld@22.1.5"

cp "$state_file" "$TMP_ROOT/state.valid"

assert_invalid_state_rejected() {
    name=$1
    output_file=$TMP_ROOT/$name.out

    run_cup_expect_failure "$output_file" doctor
    assert_contains "$(cat "$output_file")" 'state.txt'
    cp "$TMP_ROOT/state.valid" "$state_file"
    assert_cup_healthy
}

first_installed=$(grep '^installed\.' "$state_file" | sed -n '1p')
printf '%s\n' "$first_installed" >> "$state_file"
assert_invalid_state_rejected duplicate-installed-state

first_default=$(grep '^default\.' "$state_file" | sed -n '1p')
printf '%s\n' "$first_default" >> "$state_file"
assert_invalid_state_rejected duplicate-default-state

awk '
    /^default\.compiler\./ {
        sub(/=.*/, "=clang@99.0.0")
    }
    { print }
' "$state_file" > "$state_file.tmp"
mv "$state_file.tmp" "$state_file"
assert_invalid_state_rejected orphan-default-state

printf 'unexpected.key=value\n' >> "$state_file"
assert_invalid_state_rejected unknown-state-key

printf 'State persistence tests passed for %s.\n' "$TEST_PLATFORM"
