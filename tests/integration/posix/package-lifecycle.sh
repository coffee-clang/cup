#!/bin/sh

# Purpose: Owns the public install, catalog, default, update, inspect and remove
# workflow. State-file schema and malformed persistence belong to state.sh.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin package-lifecycle
prepare_command_environment

# Shared catalog and package fixtures for the public lifecycle.
prepare_fixture() {
    package_catalog_add_version compiler clang "$TEST_PLATFORM" 21.1.5
    run_cup repair >/dev/null

    make_package compiler clang 21.1.5 "$TEST_PLATFORM" clang clang++
    make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++
    make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb

    if [ "$TEST_PLATFORM" = linux-x64 ]; then
        make_package compiler gcc 16.1.0-rev1 windows-x64 gcc g++
    fi
}

# Installation, catalog and default behavior.
test_first_default() {
    output=$(run_cup install compiler clang@21.1.5)
    assert_contains "$output" 'set it as the first default'
    assert_equals "$(run_native_wrapper clang)" \
        "clang-21.1.5-$TEST_PLATFORM:clang"

    run_cup install debugger lldb@stable >/dev/null

    if [ "$TEST_PLATFORM" = linux-x64 ]; then
        run_cup install compiler gcc@stable --target windows-x64 >/dev/null
    fi
}

test_catalog_views() {
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
}

test_missing_default() {
    run_cup_expect_failure "$TMP_ROOT/default-uninstalled.out" \
        default compiler clang@stable
    assert_contains "$(cat "$TMP_ROOT/default-uninstalled.out")" \
        'is not installed'
}

# Stable updates retain old versions and move only matching defaults.
test_update_default() {
    output=$(run_cup update clang)
    assert_contains "$output" '1 stable package(s) installed, 1 default(s) moved'
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/21.1.5/bin/clang"
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/bin/clang"
    assert_contains "$(run_cup info compiler)" \
        "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"
    assert_equals "$(run_native_wrapper clang)" \
        "clang-22.1.5-$TEST_PLATFORM:clang"

    package_info=$(run_cup inspect compiler clang@stable)
    assert_contains "$package_info" \
        'Package information for compiler clang@stable -> clang@22.1.5'
    assert_contains "$package_info" 'component          compiler'
    assert_contains "$package_info" 'version            22.1.5'
}

test_update_idempotent() {
    run_cup default compiler clang@21.1.5 >/dev/null
    assert_contains "$(run_cup info compiler)" \
        "compiler [$TEST_PLATFORM]: clang@21.1.5"
    run_cup default compiler clang@stable >/dev/null

    output=$(run_cup update clang)
    assert_contains "$output" '0 stable package(s) installed, 0 default(s) moved'
}

# Cross-target, development-update and removal boundaries.
test_target_scopes() {
    [ "$TEST_PLATFORM" = linux-x64 ] || return 0

    all_installed=$(run_cup list)
    assert_contains "$all_installed" 'compiler:gcc@16.1.0-rev1 [target windows-x64]'

    native_installed=$(run_cup list --target "$TEST_PLATFORM")
    assert_contains "$native_installed" 'compiler:clang@22.1.5'
    assert_not_contains "$native_installed" 'compiler:gcc@16.1.0-rev1'

    cross_installed=$(run_cup list compiler --target windows-x64)
    assert_contains "$cross_installed" 'compiler:gcc@16.1.0-rev1'
    assert_not_contains "$cross_installed" 'compiler:clang@22.1.5'

    cross=$(run_cup info --target windows-x64)
    assert_contains "$cross" 'compiler [windows-x64]: gcc@16.1.0-rev1 (stable)'
    assert_contains "$cross" 'commands: windows-x64-gcc, windows-x64-g++'
    assert_equals "$(run_native_wrapper windows-x64-gcc)" \
        'gcc-16.1.0-rev1-windows-x64:gcc'
}

test_dev_cup_update() {
    embedded_version=$(run_cup --version)
    case "$embedded_version" in
        *-dev*)
            run_cup_expect_failure "$TMP_ROOT/cup-update-development.out" update cup
            assert_contains "$(cat "$TMP_ROOT/cup-update-development.out")" \
                'available only from an official cup release'
            ;;
    esac
}

test_remove_version() {
    run_cup remove compiler clang@21.1.5 >/dev/null
    assert_missing "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/21.1.5"
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5/info.txt"
    assert_cup_healthy
}

prepare_fixture
test_first_default
test_catalog_views
test_missing_default
test_update_default
test_update_idempotent
test_target_scopes
test_dev_cup_update
test_remove_version

printf 'Package lifecycle tests passed for %s.\n' "$TEST_PLATFORM"
