#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

test_begin state
prepare_command_environment
manifest_add_version compiler clang "$TEST_PLATFORM" 21.1.5
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
installed_count=$(grep -c '^installed\.' "$state_file")
default_count=$(grep -c '^default\.' "$state_file")
assert_equals "$installed_count" '4'
assert_equals "$default_count" '3'

installed=$(run_cup list)
assert_contains "$installed" 'compiler:clang@21.1.5'
assert_contains "$installed" 'compiler:clang@22.1.5'
assert_contains "$installed" 'debugger:lldb@22.1.5'
assert_contains "$installed" 'linker:lld@22.1.5'

current=$(run_cup current)
assert_contains "$current" "compiler [$TEST_PLATFORM]: clang@21.1.5"
assert_contains "$current" "debugger [$TEST_PLATFORM]: lldb@22.1.5"
assert_contains "$current" "linker [$TEST_PLATFORM]: lld@22.1.5"

run_cup default compiler clang@stable >/dev/null
assert_contains "$(run_cup current compiler)" \
    "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"

cp "$state_file" "$TMP_ROOT/state.valid"
first_installed=$(grep '^installed\.' "$state_file" | sed -n '1p')
printf '%s\n' "$first_installed" >> "$state_file"
run_cup_expect_failure "$TMP_ROOT/duplicate-state.out" doctor
assert_contains "$(cat "$TMP_ROOT/duplicate-state.out")" 'state.txt'
cp "$TMP_ROOT/state.valid" "$state_file"
run_cup doctor >/dev/null

printf 'State persistence tests passed for %s.\n' "$TEST_PLATFORM"
