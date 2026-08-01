#!/bin/sh

# Purpose: Exercises public CLI dispatch, help aliases and stable exit statuses.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin cli-contract
prepare_command_environment

# Status helpers run commands in isolated homes and preserve stderr for contract checks.
expect_status() (
    expected=$1 output_file=$2
    shift 2
    if run_cup "$@" >"$output_file" 2>&1; then
        status=0
    else
        status=$?
    fi
    [ "$status" -eq "$expected" ] ||
        fail "cup $* returned status $status, expected $expected"
)

run_fresh_status() (
    expected=$1 home=$2 output_file=$3
    shift 3
    if (cd "$DEV_ROOT" && HOME="$home" "$CUP" "$@") >"$output_file" 2>&1; then
        status=0
    else
        status=$?
    fi
    [ "$status" -eq "$expected" ] ||
        fail "fresh-home cup $* returned status $status, expected $expected"
)

# Public dispatch and detailed help aliases.
test_dispatch_status() {
    expect_status 2 "$TMP_ROOT/no-command.out"
    assert_contains "$(cat "$TMP_ROOT/no-command.out")" 'Usage:'

    expect_status 2 "$TMP_ROOT/unknown-command.out" unknown-command
    assert_contains "$(cat "$TMP_ROOT/unknown-command.out")" \
        "unknown command 'unknown-command'"

    expect_status 2 "$TMP_ROOT/case-command.out" Help
    assert_contains "$(cat "$TMP_ROOT/case-command.out")" "unknown command 'Help'"

    expect_status 2 "$TMP_ROOT/install-missing.out" install
    expect_status 2 "$TMP_ROOT/update-extra.out" update compiler extra
    expect_status 2 "$TMP_ROOT/config-extra.out" config set compiler clang extra
}

test_invalid_syntax() {
    # Every public parser rejects excess or missing operands through the same
    # stable usage status without reaching command side effects.
    for case_name in \
        'help-extra:help search extra' \
        'search-extra:search compiler extra' \
        'list-extra:list compiler extra' \
        'install-extra:install compiler clang extra' \
        'remove-missing:remove' \
        'remove-extra:remove compiler clang extra' \
        'default-missing:default compiler' \
        'default-extra:default compiler clang extra' \
        'info-extra:info compiler extra' \
        'inspect-missing:inspect compiler' \
        'inspect-extra:inspect compiler clang@1 extra' \
        'doctor-extra:doctor extra' \
        'repair-extra:repair extra' \
        'uninstall-extra:uninstall extra'; do
        name=${case_name%%:*}
        arguments=${case_name#*:}
        # Intentional splitting: each table entry is fixed test input.
        expect_status 2 "$TMP_ROOT/$name.out" $arguments
        assert_contains "$(cat "$TMP_ROOT/$name.out")" 'Usage:'
    done

    expect_status 2 "$TMP_ROOT/help-unknown.out" help unknown-command
    assert_contains "$(cat "$TMP_ROOT/help-unknown.out")" \
        "unknown command 'unknown-command'"

    selector_home=$TMP_ROOT/selector-home
    mkdir -p "$selector_home"
    run_fresh_status 2 "$selector_home" "$TMP_ROOT/inspect-empty-tool.out" \
        inspect compiler @stable
    run_fresh_status 2 "$selector_home" "$TMP_ROOT/default-empty-tool.out" \
        default compiler @stable
    run_fresh_status 2 "$selector_home" "$TMP_ROOT/remove-empty-tool.out" \
        remove @stable
    run_fresh_status 2 "$selector_home" "$TMP_ROOT/remove-component-empty-tool.out" \
        remove compiler @stable
    run_fresh_status 3 "$selector_home" "$TMP_ROOT/inspect-normalized.out" \
        inspect COMPILER CLANG@STABLE
    assert_missing "$selector_home/.cup"
    assert_missing "$selector_home/.coffee-cup"
}

test_help_aliases() {
    assert_contains "$(run_cup -h)" 'Commands:'
    assert_contains "$(run_cup --help)" 'Commands:'
    assert_contains "$(run_cup help)" 'install      Install one package, profile or toolchain.'

    for command in help search list install remove update config default info inspect \
        doctor repair uninstall; do
        for form in "help $command" "$command -h" "$command --help"; do
            # Intentional splitting: every form contains only fixed test words.
            output=$(run_cup $form)
            assert_contains "$output" 'Usage:'
            assert_contains "$output" 'Description:'
            assert_contains "$output" 'Arguments:'
            assert_contains "$output" 'Options:'
            assert_contains "$output" 'Defaults:'
            assert_contains "$output" 'Examples:'
            assert_contains "$output" 'Effects:'
        done
    done

    output=$(run_cup help update)
    assert_contains "$output" \
        'Without a selector, updates installed tools only; cup itself is not updated.'
    output=$(run_cup help install)
    assert_contains "$output" "cup install <profile|toolchain> <name>"
    assert_contains "$output" "cup install [<component>] <tool>[@<release>]"
    assert_not_contains "$output" '| install <profile|toolchain>'
    assert_contains "$output" 'Select tar.xz, tar.gz or zip.'
    output=$(run_cup help remove)
    assert_contains "$output" "cup remove [<component>] <tool>[@<release>]"
    assert_not_contains "$output" 'profile'
    assert_not_contains "$output" 'toolchain'
    output=$(run_cup help config)
    assert_contains "$output" "cup config set <component> <tool>"
    assert_contains "$output" 'reset without component clears that scope only.'
    output=$(run_cup help uninstall)
    assert_contains "$output" '--yes  Skip the confirmation prompt.'

}

# Read-only initialization and persistent-state status mapping.
test_read_only_no_init() {
    fresh_home=$TMP_ROOT/read-only-home
    mkdir -p "$fresh_home"

    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-help.out" help
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-version.out" --version
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-search.out" search compiler
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-list.out" list
    assert_contains "$(cat "$TMP_ROOT/fresh-list.out")" 'No packages installed'
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-info.out" info
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-config.out" config
    assert_missing "$fresh_home/.cup"
    run_fresh_status 0 "$fresh_home" "$TMP_ROOT/fresh-doctor.out" doctor
    assert_contains "$(cat "$TMP_ROOT/fresh-doctor.out")" 'runtime is not initialized'
    assert_missing "$fresh_home/.cup"
    run_fresh_status 3 "$fresh_home" "$TMP_ROOT/fresh-inspect.out" \
        inspect compiler clang@1.0.0
    assert_missing "$fresh_home/.cup"
}

test_state_status() {
    state_home=$TMP_ROOT/state-home
    mkdir -p "$state_home"
    (cd "$DEV_ROOT" && HOME="$state_home" "$CUP" repair) >/dev/null
    printf 'not-a-state-record\n' > "$state_home/.cup/state.txt"
    run_fresh_status 4 "$state_home" "$TMP_ROOT/invalid-state.out" list
}

test_root_selection() {
    foreign_home=$TMP_ROOT/foreign-root-home
    mkdir -p "$foreign_home/.cup"
    printf 'unrelated\n' > "$foreign_home/.cup/foreign.txt"
    run_fresh_status 0 "$foreign_home" "$TMP_ROOT/foreign-root.out" repair
    assert_file "$foreign_home/.cup/foreign.txt"
    assert_file "$foreign_home/.coffee-cup/root.txt"
    assert_file "$foreign_home/.coffee-cup/state.txt"
    assert_equals "$(sed -n '1p' "$foreign_home/.coffee-cup/root.txt")" 'format=1'
    assert_equals "$(sed -n '2p' "$foreign_home/.coffee-cup/root.txt")" \
        'product=coffee-clang/cup'
    assert_equals "$(sed -n '3p' "$foreign_home/.coffee-cup/root.txt")" 'layout=1'

    legacy_home=$TMP_ROOT/legacy-root-home
    mkdir -p "$legacy_home/.cup/components" "$legacy_home/.cup/staging" \
        "$legacy_home/.cup/cache"
    printf 'format=1\n' > "$legacy_home/.cup/state.txt"
    state_hash=$(hash_file "$legacy_home/.cup/state.txt")
    run_fresh_status 0 "$legacy_home" "$TMP_ROOT/legacy-root.out" repair
    assert_equals "$(hash_file "$legacy_home/.cup/state.txt")" "$state_hash"
    assert_missing "$legacy_home/.cup/root.txt"
    assert_file "$legacy_home/.coffee-cup/root.txt"

    verified_home=$TMP_ROOT/verified-legacy-root-home
    verified_root=$verified_home/.cup
    mkdir -p "$verified_root/bin" "$verified_root/components" \
        "$verified_root/staging" "$verified_root/cache" \
        "$verified_root/config" "$verified_root/helpers"
    cp "$CUP" "$verified_root/bin/cup"
    cp "$CUP" "$verified_root/helpers/cup-update-helper"
    cp "$DEV_ROOT/scripts/install/uninstall-cup.sh" "$verified_root/helpers/uninstall.sh"
    cp "$DEV_ROOT/config/packages.cfg" "$verified_root/config/packages.cfg"
    cp "$DEV_ROOT/config/install.cfg" "$verified_root/config/install.cfg"
    chmod +x "$verified_root/bin/cup" "$verified_root/helpers/cup-update-helper" \
        "$verified_root/helpers/uninstall.sh"
    printf 'format=1\n' > "$verified_root/state.txt"
    {
        printf '%s  packages.cfg\n' "$(hash_file "$verified_root/config/packages.cfg")"
        printf '%s  install.cfg\n' "$(hash_file "$verified_root/config/install.cfg")"
        printf '%s  install.sh\n' "$(hash_file "$PROJECT_ROOT/scripts/install/install-cup.sh")"
        printf '%s  install.ps1\n' "$(hash_file "$PROJECT_ROOT/scripts/install/install-cup-windows.ps1")"
    } > "$verified_root/config/SHA256SUMS.common"
    {
        printf '%s  cup-linux-x64\n' "$(hash_file "$verified_root/bin/cup")"
        printf '%s  uninstall.sh\n' "$(hash_file "$verified_root/helpers/uninstall.sh")"
        printf '%s  release.txt\n' \
            "$(hash_text 'format=1\nversion=0.2.1\ncommit=0000000000000000000000000000000000000000\n')"
    } > "$verified_root/config/SHA256SUMS.linux-x64"
    run_fresh_status 0 "$verified_home" "$TMP_ROOT/verified-legacy-root.out" repair
    assert_file "$verified_root/root.txt"
    assert_missing "$verified_home/.coffee-cup"

    lookalike_home=$TMP_ROOT/lookalike-root-home
    mkdir -p "$lookalike_home/.cup/components" "$lookalike_home/.cup/staging" \
        "$lookalike_home/.cup/cache"
    printf 'not-a-cup-state\n' > "$lookalike_home/.cup/state.txt"
    state_hash=$(hash_file "$lookalike_home/.cup/state.txt")
    run_fresh_status 0 "$lookalike_home" "$TMP_ROOT/lookalike-root.out" repair
    assert_equals "$(hash_file "$lookalike_home/.cup/state.txt")" "$state_hash"
    assert_missing "$lookalike_home/.cup/root.txt"
    assert_file "$lookalike_home/.coffee-cup/root.txt"

    damaged_home=$TMP_ROOT/damaged-legacy-root-home
    mkdir -p "$damaged_home/.cup/bin"
    printf 'not-a-verified-cup-generation\n' > "$damaged_home/.cup/bin/cup"
    binary_hash=$(hash_file "$damaged_home/.cup/bin/cup")
    run_fresh_status 4 "$damaged_home" "$TMP_ROOT/damaged-root.out" repair
    assert_contains "$(cat "$TMP_ROOT/damaged-root.out")" \
        'probable legacy cup root'
    assert_equals "$(hash_file "$damaged_home/.cup/bin/cup")" "$binary_hash"
    assert_missing "$damaged_home/.cup/root.txt"
    assert_missing "$damaged_home/.coffee-cup"

    corrupt_home=$TMP_ROOT/corrupt-root-home
    mkdir -p "$corrupt_home"
    run_fresh_status 0 "$corrupt_home" "$TMP_ROOT/corrupt-root-setup.out" repair
    state_hash=$(hash_file "$corrupt_home/.cup/state.txt")
    printf 'corrupt
' > "$corrupt_home/.cup/root.txt"
    marker_hash=$(hash_file "$corrupt_home/.cup/root.txt")
    run_fresh_status 4 "$corrupt_home" "$TMP_ROOT/corrupt-root-doctor.out" doctor
    assert_contains "$(cat "$TMP_ROOT/corrupt-root-doctor.out")"         'cup root marker is invalid for recognized root'
    assert_contains "$(cat "$TMP_ROOT/corrupt-root-doctor.out")"         'neither cup root candidate was selected or modified'
    assert_equals "$(hash_file "$corrupt_home/.cup/state.txt")" "$state_hash"
    assert_equals "$(hash_file "$corrupt_home/.cup/root.txt")" "$marker_hash"
    assert_missing "$corrupt_home/.coffee-cup"
    run_fresh_status 4 "$corrupt_home" "$TMP_ROOT/corrupt-root-repair.out" repair
    assert_missing "$corrupt_home/.coffee-cup"
}

test_root_home() {
    if (cd "$DEV_ROOT" && HOME=/ "$CUP" doctor) >"$TMP_ROOT/root-home.out" 2>&1; then
        fail 'HOME=/ was accepted'
    fi
    assert_contains "$(cat "$TMP_ROOT/root-home.out")" \
        'HOME must not be the filesystem root'
}

test_dispatch_status
test_invalid_syntax
test_help_aliases
test_read_only_no_init
test_state_status
test_root_selection
test_root_home
printf 'CLI contract tests passed for %s.\n' "$TEST_PLATFORM"
