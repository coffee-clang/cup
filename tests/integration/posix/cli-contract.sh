#!/bin/sh

# Exercises public CLI dispatch, help aliases and stable exit statuses.
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

test_syntax_precedes_runtime_preflight() {
    syntax_home=$TMP_ROOT/syntax-before-journal-home
    mkdir -p "$syntax_home"
    run_fresh_status 0 "$syntax_home" "$TMP_ROOT/syntax-before-journal-setup.out" repair
    printf 'invalid journal\n' > "$syntax_home/.cup/transaction.txt"

    for case_name in \
        'missing-install:install' \
        'invalid-selector:install compiler @stable' \
        'missing-profile:install profile' \
        'invalid-target:list --target windows-arm64' \
        'invalid-config:config change compiler clang' \
        'invalid-config-selector:config set compiler clang@stable' \
        'invalid-inspect-release:inspect compiler clang@RC1' \
        'invalid-default-release:default compiler clang@../x'; do
        name=${case_name%%:*}
        arguments=${case_name#*:}
        # Intentional splitting: each table entry is fixed test input.
        run_fresh_status 2 "$syntax_home" "$TMP_ROOT/syntax-before-journal-$name.out" $arguments
        output=$(cat "$TMP_ROOT/syntax-before-journal-$name.out")
        assert_contains "$output" 'Usage:'
        assert_not_contains "$output" 'transaction journal is invalid'
    done

    long_value=$(awk 'BEGIN { for (i = 0; i < 512; ++i) printf "x" }')
    run_fresh_status 2 "$syntax_home" "$TMP_ROOT/syntax-before-journal-long.out" \
        search "$long_value"
    output=$(cat "$TMP_ROOT/syntax-before-journal-long.out")
    assert_contains "$output" 'exceed their supported length'
    assert_contains "$output" 'Usage:'
    assert_not_contains "$output" 'transaction journal is invalid'
}

# Case aliases use the same normalized action for parsing, mutation, and interrupt setup.
test_config_action_normalization() {
    config_home=$TMP_ROOT/config-action-home
    mkdir -p "$config_home"
    run_fresh_status 0 "$config_home" "$TMP_ROOT/config-action-setup.out" repair
    run_fresh_status 0 "$config_home" "$TMP_ROOT/config-set-uppercase.out" \
        config SET COMPILER CLANG
    assert_contains "$(cat "$TMP_ROOT/config-set-uppercase.out")" "set to 'clang'"
    run_fresh_status 0 "$config_home" "$TMP_ROOT/config-reset-uppercase.out" \
        config RESET COMPILER
    assert_contains "$(cat "$TMP_ROOT/config-reset-uppercase.out")" 'was reset'
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
    assert_contains "$output" 'reset without component clears preferences for that target.'
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

    unmarked_home=$TMP_ROOT/unmarked-cup-root-home
    mkdir -p "$unmarked_home/.cup/bin"
    printf 'unmarked-cup-generation\n' > "$unmarked_home/.cup/bin/cup"
    binary_hash=$(hash_file "$unmarked_home/.cup/bin/cup")
    run_fresh_status 4 "$unmarked_home" "$TMP_ROOT/unmarked-root.out" repair
    assert_contains "$(cat "$TMP_ROOT/unmarked-root.out")" \
        'unmarked cup-like root'
    assert_contains "$(cat "$TMP_ROOT/unmarked-root.out")" \
        'Move the preserved directory to a backup path'
    assert_contains "$(cat "$TMP_ROOT/unmarked-root.out")" \
        'do not add root.txt manually'
    assert_equals "$(hash_file "$unmarked_home/.cup/bin/cup")" "$binary_hash"
    assert_missing "$unmarked_home/.cup/root.txt"
    assert_missing "$unmarked_home/.coffee-cup"

    lookalike_home=$TMP_ROOT/lookalike-root-home
    mkdir -p "$lookalike_home/.cup/components" "$lookalike_home/.cup/staging" \
        "$lookalike_home/.cup/cache"
    printf 'not-a-cup-state\n' > "$lookalike_home/.cup/state.txt"
    state_hash=$(hash_file "$lookalike_home/.cup/state.txt")
    run_fresh_status 0 "$lookalike_home" "$TMP_ROOT/lookalike-root.out" repair
    assert_equals "$(hash_file "$lookalike_home/.cup/state.txt")" "$state_hash"
    assert_missing "$lookalike_home/.cup/root.txt"
    assert_file "$lookalike_home/.coffee-cup/root.txt"

    corrupt_home=$TMP_ROOT/corrupt-root-home
    mkdir -p "$corrupt_home"
    run_fresh_status 0 "$corrupt_home" "$TMP_ROOT/corrupt-root-setup.out" repair
    state_hash=$(hash_file "$corrupt_home/.cup/state.txt")
    printf 'corrupt\n' > "$corrupt_home/.cup/root.txt"
    marker_hash=$(hash_file "$corrupt_home/.cup/root.txt")
    run_fresh_status 4 "$corrupt_home" "$TMP_ROOT/corrupt-root-doctor.out" doctor
    assert_contains "$(cat "$TMP_ROOT/corrupt-root-doctor.out")" \
        'cup root marker is invalid for recognized root'
    assert_contains "$(cat "$TMP_ROOT/corrupt-root-doctor.out")" \
        'neither cup root candidate was selected or modified'
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
test_syntax_precedes_runtime_preflight
test_config_action_normalization
test_help_aliases
test_read_only_no_init
test_state_status
test_root_selection
test_root_home
printf 'CLI contract tests passed for %s.\n' "$TEST_PLATFORM"
