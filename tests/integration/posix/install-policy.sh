#!/bin/sh

# Exercises scoped install preferences, profiles and explicit toolchains.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin install-policy
prepare_command_environment

# Shared package fixture for scoped defaults and curated plans.
component_root() {
    component=$1 tool=$2 version=$3
    printf '%s/.cup/components/%s/%s/%s/%s/%s\n' \
        "$TEST_HOME" "$component" "$tool" "$TEST_PLATFORM" \
        "$TEST_PLATFORM" "$version"
}

prepare_fixture() {
    run_cup repair >/dev/null
    package_catalog_ensure_package compiler gcc "$TEST_PLATFORM" \
        16.2.0-rev1 tar.gz
    make_package compiler clang 23.1.0 "$TEST_PLATFORM" clang clang++
    make_package compiler gcc 16.2.0-rev1 "$TEST_PLATFORM" gcc g++
    make_package debugger lldb 23.1.0 "$TEST_PLATFORM" lldb
    make_package debugger gdb 17.2 "$TEST_PLATFORM" gdb
    make_package linker ld 2.47 "$TEST_PLATFORM" ld
    make_package linker lld 23.1.0 "$TEST_PLATFORM" lld
    make_package formatter clang-format 23.1.0 "$TEST_PLATFORM" clang-format
    make_package linter clang-tidy 23.1.0 "$TEST_PLATFORM" clang-tidy
    make_package language-server clangd 23.1.0 "$TEST_PLATFORM" clangd
}

# Profile/default resolution and user preference scenarios.
test_defaults_profile() {
    output=$(run_cup config)
    assert_contains "$output" \
        "Install selections for host '$TEST_PLATFORM', target '$TEST_PLATFORM'"
    assert_contains "$output" \
        'compiler           clang              clang              official default'
    assert_contains "$(run_cup config --target windows-x64)" \
        'debugger           -                  -                  unavailable'
    assert_contains "$output" 'Profiles:'
    assert_contains "$output" 'minimal      compiler, linker'
    assert_contains "$output" 'Toolchains:'
    assert_contains "$output" 'gnu          gcc, gdb, ld'

    output=$(run_cup install PROFILE MINIMAL)
    assert_contains "$output" "Installing profile 'minimal' (2 packages)"
    assert_contains "$output" \
        "Install group 'minimal' completed: 2 package(s) installed, 0 skipped."
    assert_file "$(component_root compiler clang 23.1.0)/info.txt"
    assert_file "$(component_root linker lld 23.1.0)/info.txt"
}

test_scoped_preferences() {
    assert_contains "$(run_cup config set COMPILER GCC)" \
        "Preferred tool for 'compiler' on target '$TEST_PLATFORM' set to 'gcc'."
    output=$(run_cup config)
    assert_contains "$output" \
        'compiler           gcc                clang              user preference'

    output=$(run_cup install COMPILER)
    assert_contains "$output" 'Installed compiler gcc@16.2.0-rev1'
    assert_file "$(component_root compiler gcc 16.2.0-rev1)/info.txt"

    assert_contains "$(run_cup config set compiler gcc --target windows-x64)" \
        "Preferred tool for 'compiler' on target 'windows-x64' set to 'gcc'."
    assert_contains "$(cat "$TEST_HOME/.cup/config/preferences.txt")" \
        "preferred.$TEST_PLATFORM.$TEST_PLATFORM.compiler=gcc"
    assert_contains "$(cat "$TEST_HOME/.cup/config/preferences.txt")" \
        "preferred.$TEST_PLATFORM.windows-x64.compiler=gcc"

    printf '# preserve on no-op reset\n' >> "$TEST_HOME/.cup/config/preferences.txt"
    assert_contains "$(run_cup config reset debugger --target windows-x64)" \
        "No preference was set for 'debugger' on target 'windows-x64'."
    assert_contains "$(cat "$TEST_HOME/.cup/config/preferences.txt")" \
        '# preserve on no-op reset'

    assert_contains "$(run_cup config reset compiler)" \
        "Preference for 'compiler' on target '$TEST_PLATFORM' was reset."
    assert_not_contains "$(cat "$TEST_HOME/.cup/config/preferences.txt")" \
        "preferred.$TEST_PLATFORM.$TEST_PLATFORM.compiler="
    assert_contains "$(cat "$TEST_HOME/.cup/config/preferences.txt")" \
        "preferred.$TEST_PLATFORM.windows-x64.compiler=gcc"

    assert_contains "$(run_cup config reset --target windows-x64)" \
        "Reset 1 preference(s) for target 'windows-x64'."
    assert_missing "$TEST_HOME/.cup/config/preferences.txt"
}

# GNU is complete on the native Linux scopes. On macOS, group preflight must
# still reject the deliberately incomplete catalog before installing a member.
test_gnu_toolchain() {
    case "$TEST_PLATFORM" in
        linux-*)
            output=$(run_cup install TOOLCHAIN GNU)
            assert_contains "$output" "Installing toolchain 'gnu' (3 packages)"
            assert_contains "$output" \
                "Install group 'gnu' completed: 2 package(s) installed, 1 skipped."
            assert_file "$(component_root debugger gdb 17.2)/info.txt"
            assert_file "$(component_root linker ld 2.47)/info.txt"
            ;;
        macos-*)
            run_cup_expect_failure "$TMP_ROOT/gnu-toolchain.out" install TOOLCHAIN GNU
            output=$(cat "$TMP_ROOT/gnu-toolchain.out")
            assert_contains "$output" "Install group 'gnu' cannot be installed"
            assert_contains "$output" 'gdb                not currently available'
            assert_contains "$output" 'ld                 not currently available'
            assert_contains "$output" 'No packages were installed.'
            assert_missing "$(component_root debugger gdb 17.2)"
            assert_missing "$(component_root linker ld 2.47)"
            ;;
    esac
}

test_toolchain_explicit() {
    mkdir -p "$TEST_HOME/.cup/config"
    printf 'format=broken\npreset=gnu\n' > "$TEST_HOME/.cup/config/preferences.txt"

    output=$(run_cup install TOOLCHAIN LLVM)
    assert_contains "$output" "Installing toolchain 'llvm' (6 packages)"
    assert_contains "$output" "Install group 'llvm' completed: 4 package(s) installed, 2 skipped."
    assert_file "$(component_root debugger lldb 23.1.0)/info.txt"
    assert_file "$(component_root formatter clang-format 23.1.0)/info.txt"
    assert_file "$(component_root linter clang-tidy 23.1.0)/info.txt"
    assert_file "$(component_root language-server clangd 23.1.0)/info.txt"
    rm -f "$TEST_HOME/.cup/config/preferences.txt"
}

prepare_fixture
test_defaults_profile
test_scoped_preferences
test_gnu_toolchain
test_toolchain_explicit
assert_cup_healthy
printf 'Install policy tests passed for %s.\n' "$TEST_PLATFORM"
