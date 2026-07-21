#!/bin/sh

# Purpose: Owns public CLI dispatch, help aliases and stable exit statuses.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin cli-contract
prepare_command_environment

expect_status() (
    expected=$1 output_file=$2
    shift 2
    if run_cup "$@" >"$output_file" 2>&1; then status=0; else status=$?; fi
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
    assert_contains "$output" 'Without a selector, updates installed tools only; CUP itself is not updated.'
    output=$(run_cup help config)
    assert_contains "$output" 'reset without component clears that scope only.'
    output=$(run_cup help uninstall)
    assert_contains "$output" '--yes  Skip the confirmation prompt.'

    expect_status 2 "$TMP_ROOT/help-current.out" help current
    expect_status 2 "$TMP_ROOT/help-self-update.out" help self-update
    assert_contains "$(cat "$TMP_ROOT/help-current.out")" "unknown command 'current'"
    assert_contains "$(cat "$TMP_ROOT/help-self-update.out")" "unknown command 'self-update'"
}

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

test_root_home() {
    if (cd "$DEV_ROOT" && HOME=/ "$CUP" doctor) >"$TMP_ROOT/root-home.out" 2>&1; then
        fail 'HOME=/ was accepted'
    fi
    assert_contains "$(cat "$TMP_ROOT/root-home.out")" \
        'HOME must not be the filesystem root'
}

test_dispatch_status
test_help_aliases
test_read_only_no_init
test_state_status
test_root_home
printf 'CLI contract tests passed for %s.\n' "$TEST_PLATFORM"
