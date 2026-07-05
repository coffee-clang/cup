#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin entrypoints
prepare_command_environment
run_cup repair >/dev/null

make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++
make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb
run_cup install compiler clang@stable >/dev/null
run_cup install debugger lldb@stable >/dev/null

assert_equals "$(run_native_entrypoint clang)" \
    "clang-22.1.5-$TEST_PLATFORM:clang"
assert_equals "$(run_native_entrypoint lldb)" \
    "lldb-22.1.5-$TEST_PLATFORM:lldb"

printf 'altered\n' > "$(native_entrypoint_path clang)"
run_cup_expect_failure "$TMP_ROOT/current-altered.out" info
assert_contains "$(cat "$TMP_ROOT/current-altered.out")" 'status: invalid'
run_cup_expect_failure "$TMP_ROOT/doctor-altered.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-altered.out")" 'entry point'
run_cup repair >/dev/null
assert_equals "$(run_native_entrypoint clang)" \
    "clang-22.1.5-$TEST_PLATFORM:clang"

stale=$TEST_HOME/.cup/bin/stale-command
printf '#!/bin/sh\nexit 0\n' > "$stale"
chmod +x "$stale"
run_cup_expect_failure "$TMP_ROOT/doctor-stale.out" doctor
assert_contains "$(cat "$TMP_ROOT/doctor-stale.out")" 'stale or unmanaged entry point'
run_cup repair >/dev/null
assert_missing "$stale"

make_package linker lld 22.1.5 "$TEST_PLATFORM" cup
run_cup_expect_failure "$TMP_ROOT/reserved-entry.out" \
    install linker lld@stable
assert_contains "$(cat "$TMP_ROOT/reserved-entry.out")" 'conflicts with cup itself'
assert_not_contains "$(run_cup list)" 'linker:lld@22.1.5'

if [ "${TEST_PLATFORM%%-*}" = macos ]; then
    make_package formatter clang-format 22.1.5 "$TEST_PLATFORM" CLANG
    run_cup_expect_failure "$TMP_ROOT/case-collision.out" \
        install formatter clang-format@stable
    assert_contains "$(cat "$TMP_ROOT/case-collision.out")" \
        'declared by more than one default package'
fi

run_cup doctor >/dev/null
printf 'Entry point tests passed for %s.\n' "$TEST_PLATFORM"
