#!/bin/sh

# Exercises the public install, catalog, default, update, inspect and remove
# workflow. State-file schema and malformed persistence belong to state.sh.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin package-lifecycle
prepare_command_environment

# Shared catalog and package fixtures for the public lifecycle.
prepare_fixture() {
    package_catalog_edit compiler clang "$TEST_PLATFORM" available_versions 21.1.5 prepend
    package_catalog_edit debugger lldb "$TEST_PLATFORM" available_versions 21.1.5 prepend
    run_cup repair >/dev/null

    make_package compiler clang 21.1.5 "$TEST_PLATFORM" clang clang++
    make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++
    make_package debugger lldb 21.1.5 "$TEST_PLATFORM" lldb
    make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb

    if [ "$TEST_PLATFORM" = linux-x64 ]; then
        make_package compiler gcc 16.1.0-rev1 windows-x64 gcc g++
    fi
}

# Installation, catalog and default behavior.
test_install_defaults() {
    output=$(run_cup install clang@21.1.5)
    assert_contains "$output" 'set it as the first default'
    assert_equals "$(run_native_wrapper clang)" \
        "clang-21.1.5-$TEST_PLATFORM:clang"

    state_hash=$(hash_file "$TEST_HOME/.cup/state.txt")
    wrapper_hash=$(hash_file "$(native_wrapper_path clang)")
    reinstall=$(run_cup install compiler clang@21.1.5 2>&1)
    assert_contains "$reinstall" \
        "Package 'compiler:clang@21.1.5' is already installed"
    assert_contains "$reinstall" 'no changes were made.'
    assert_not_contains "$reinstall" 'Error:'
    assert_equals "$(hash_file "$TEST_HOME/.cup/state.txt")" "$state_hash"
    assert_equals "$(hash_file "$(native_wrapper_path clang)")" "$wrapper_hash"
    assert_missing "$TEST_HOME/.cup/transaction.txt"
    if find "$TEST_HOME/.cup/staging" -mindepth 1 -print -quit | grep . >/dev/null; then
        fail 'idempotent reinstall created staging content'
    fi

    second=$(run_cup install compiler clang@22.1.5)
    assert_not_contains "$second" 'set it as the first default'
    assert_contains "$(run_cup info compiler)" \
        "compiler [$TEST_PLATFORM]: clang@21.1.5"
    assert_equals "$(run_native_wrapper clang)" \
        "clang-21.1.5-$TEST_PLATFORM:clang"

    run_cup install debugger lldb@21.1.5 >/dev/null

    if [ "$TEST_PLATFORM" = linux-x64 ]; then
        run_cup install compiler gcc@stable --target windows-x64 >/dev/null
    fi
}

test_catalog_views() {
    info_output=$(run_cup info)
    assert_contains "$info_output" "compiler [$TEST_PLATFORM]: clang@21.1.5"
    assert_contains "$info_output" "debugger [$TEST_PLATFORM]: lldb@21.1.5"
    assert_contains "$info_output" 'commands: clang, clang++'
    assert_contains "$info_output" 'status: default'

    component_info=$(run_cup info compiler)
    assert_contains "$component_info" "compiler [$TEST_PLATFORM]: clang@21.1.5"
    assert_not_contains "$component_info" "debugger [$TEST_PLATFORM]"

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
    assert_contains "$installed" 'compiler:clang@22.1.5'
    assert_not_contains "$installed" 'debugger:lldb@21.1.5'
}

test_missing_default() {
    run_cup_expect_failure "$TMP_ROOT/default-uninstalled.out" \
        default compiler clang@20.1.5
    assert_contains "$(cat "$TMP_ROOT/default-uninstalled.out")" \
        'is not installed'
}

# Updates retain old versions and move only defaults selecting the same tool.
test_updates() {
    component_update=$(run_cup update compiler)
    assert_contains "$component_update" \
        '0 stable package(s) installed, 1 default(s) moved'
    assert_contains "$(run_cup info compiler)" \
        "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"
    assert_equals "$(run_native_wrapper clang)" \
        "clang-22.1.5-$TEST_PLATFORM:clang"

    global_update=$(run_cup update)
    assert_contains "$global_update" \
        '1 stable package(s) installed, 1 default(s) moved'
    assert_file "$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/"\
"$TEST_PLATFORM/21.1.5/bin/lldb"
    assert_file "$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/"\
"$TEST_PLATFORM/22.1.5/bin/lldb"
    assert_contains "$(run_cup info debugger)" \
        "debugger [$TEST_PLATFORM]: lldb@22.1.5 (stable)"
    assert_equals "$(run_native_wrapper lldb)" \
        "lldb-22.1.5-$TEST_PLATFORM:lldb"
    assert_missing "$TEST_HOME/.cup/transaction.txt"

    package_info=$(run_cup inspect compiler clang@stable)
    assert_contains "$package_info" \
        'Package information for compiler clang@stable -> clang@22.1.5'
    assert_contains "$package_info" 'component          compiler'
    assert_contains "$package_info" 'version            22.1.5'

    run_cup default compiler clang@21.1.5 >/dev/null
    assert_contains "$(run_cup info compiler)" \
        "compiler [$TEST_PLATFORM]: clang@21.1.5"
    run_cup default compiler clang@stable >/dev/null

    idempotent=$(run_cup update clang)
    assert_contains "$idempotent" \
        '0 stable package(s) installed, 0 default(s) moved'
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

test_dev_update() {
    embedded_version=$(run_cup --version)
    case "$embedded_version" in
        *-dev*)
            run_cup_expect_failure "$TMP_ROOT/cup-update-development.out" update cup
            assert_contains "$(cat "$TMP_ROOT/cup-update-development.out")" \
                'available only from an official cup release'
            ;;
    esac
}

test_remove_default_without_promotion() {
    run_cup_expect_status "$TMP_ROOT/remove-ambiguous.out" 2 remove clang
    ambiguous=$(cat "$TMP_ROOT/remove-ambiguous.out")
    assert_contains "$ambiguous" "remove selection 'compiler:clang' is ambiguous"
    assert_contains "$ambiguous" 'clang@21.1.5'
    assert_contains "$ambiguous" 'clang@22.1.5'
    assert_contains "$ambiguous" 'Specify one of the installed releases with:'
    assert_contains "$ambiguous" \
        "cup remove compiler clang@<release> --target $TEST_PLATFORM"
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/"\
"$TEST_PLATFORM/21.1.5/info.txt"
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/"\
"$TEST_PLATFORM/22.1.5/info.txt"

    run_cup remove compiler clang@stable >/dev/null
    assert_missing "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5"
    assert_file "$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/"\
"$TEST_PLATFORM/21.1.5/info.txt"
    assert_missing "$(native_wrapper_path clang)"
    assert_missing "$(native_wrapper_path clang++)"
    assert_contains "$(run_cup info compiler --target "$TEST_PLATFORM")" \
        "No default for component 'compiler' on host '$TEST_PLATFORM', target '$TEST_PLATFORM'."
    native_installed=$(run_cup list compiler --target "$TEST_PLATFORM")
    assert_contains "$native_installed" 'compiler:clang@21.1.5'
    assert_not_contains "$native_installed" 'compiler:clang@22.1.5'
    assert_cup_healthy

    assert_contains "$(run_cup remove clang)" 'Removed compiler clang -> clang@21.1.5'
    assert_not_contains "$(run_cup list compiler --target "$TEST_PLATFORM")" 'compiler:clang@'
    assert_cup_healthy
}

prepare_fixture
test_install_defaults
test_catalog_views
test_missing_default
test_updates
test_target_scopes
test_dev_update
test_remove_default_without_promotion

printf 'Package lifecycle tests passed for %s.\n' "$TEST_PLATFORM"
