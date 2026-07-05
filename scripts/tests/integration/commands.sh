#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin commands
prepare_command_environment
manifest_add_version compiler clang "$TEST_PLATFORM" 21.1.5
run_cup repair >/dev/null

make_package compiler clang 21.1.5 "$TEST_PLATFORM" clang clang++
make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++
make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb

output=$(run_cup install compiler clang@21.1.5)
assert_contains "$output" 'set it as the first default'
run_cup install debugger lldb@stable >/dev/null

if [ "$TEST_PLATFORM" = linux-x64 ]; then
    make_package compiler gcc 16.1.0-rev1 windows-x64 gcc g++
    run_cup install compiler gcc@stable --target windows-x64 >/dev/null
fi

run_cup_expect_failure "$TMP_ROOT/default-missing.out" default
assert_contains "$(cat "$TMP_ROOT/default-missing.out")" \
    'missing option <component>'

run_cup_expect_failure "$TMP_ROOT/default-uninstalled.out" \
    default compiler clang@stable
assert_contains "$(cat "$TMP_ROOT/default-uninstalled.out")" 'is not installed'

embedded_version=$(run_cup --version)
case "$embedded_version" in
    *-dev*)
        run_cup_expect_failure "$TMP_ROOT/self-update-development.out" self-update
        assert_contains "$(cat "$TMP_ROOT/self-update-development.out")" \
            'available only from an official cup release'
        ;;
esac

current=$(run_cup info)
assert_contains "$current" "compiler [$TEST_PLATFORM]: clang@21.1.5"
assert_contains "$current" "debugger [$TEST_PLATFORM]: lldb@22.1.5 (stable)"
assert_contains "$current" 'commands: clang, clang++'
assert_contains "$current" 'status: active'

component_current=$(run_cup info compiler)
assert_contains "$component_current" "compiler [$TEST_PLATFORM]: clang@21.1.5"
assert_not_contains "$component_current" "debugger [$TEST_PLATFORM]"

catalog=$(run_cup search)
assert_contains "$catalog" "Available packages for host '$TEST_PLATFORM'"
assert_contains "$catalog" 'compiler:'
assert_contains "$catalog" 'clang'
component_catalog=$(run_cup search compiler)
assert_contains "$component_catalog" "Available tools for component 'compiler'"
assert_contains "$component_catalog" 'clang'
assert_not_contains "$component_catalog" 'debugger:'

installed=$(run_cup list compiler)
assert_contains "$installed" 'compiler:clang@21.1.5'
assert_not_contains "$installed" 'debugger:lldb@22.1.5'

entry_output=$(run_native_entrypoint clang)
assert_equals "$entry_output" "clang-21.1.5-$TEST_PLATFORM:clang"

output=$(run_cup update clang)
assert_contains "$output" '1 stable package(s) installed, 1 default(s) moved'
assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/21.1.5/bin/clang"
assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/bin/clang"
assert_contains "$(run_cup info compiler)" \
    "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"
assert_equals "$(run_native_entrypoint clang)" \
    "clang-22.1.5-$TEST_PLATFORM:clang"

package_info=$(run_cup inspect compiler clang@stable)
assert_contains "$package_info" 'Package information for compiler clang@stable -> clang@22.1.5'
assert_contains "$package_info" 'component          compiler'
assert_contains "$package_info" 'version            22.1.5'

run_cup default compiler clang@21.1.5 >/dev/null
assert_contains "$(run_cup info compiler)" \
    "compiler [$TEST_PLATFORM]: clang@21.1.5"
run_cup default compiler clang@stable >/dev/null

output=$(run_cup update clang)
assert_contains "$output" '0 stable package(s) installed, 0 default(s) moved'

if [ "$TEST_PLATFORM" = linux-x64 ]; then
    cross=$(run_cup info --target windows-x64)
    assert_contains "$cross" 'compiler [windows-x64]: gcc@16.1.0-rev1 (stable)'
    assert_contains "$cross" 'commands: windows-x64-gcc, windows-x64-g++'
    assert_equals "$(run_native_entrypoint windows-x64-gcc)" \
        'gcc-16.1.0-rev1-windows-x64:gcc'
fi

run_cup remove compiler clang@21.1.5 >/dev/null
assert_missing "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/21.1.5"
run_cup doctor >/dev/null

if (cd "$DEV_ROOT" && HOME=/ "$CUP" doctor) >"$TMP_ROOT/root-home.out" 2>&1; then
    fail 'HOME=/ was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/root-home.out")" 'HOME must not be the filesystem root'

printf 'Command integration tests passed for %s.\n' "$TEST_PLATFORM"
