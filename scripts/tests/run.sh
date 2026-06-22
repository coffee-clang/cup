#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

run_suite() {
    label=$1
    script=$2
    printf '==> %s\n' "$label"
    "$SCRIPT_DIR/$script"
}

run_suite 'Testing version policy...' version.sh

if [ "${CUP_TEST_SKIP_BUILD:-0}" = 1 ]; then
    require_test_binary
else
    run_suite "Building development binaries for $TEST_PLATFORM..." build.sh
fi

run_suite 'Testing generated CA bundle...' certs.sh
run_suite 'Testing public command behavior...' commands.sh
run_suite 'Testing state persistence...' state.sh
run_suite 'Testing managed entry points...' entrypoints.sh
run_suite 'Testing transaction recovery...' recovery.sh
run_suite 'Testing detached uninstall cleanup...' uninstall.sh

printf 'All POSIX cup tests passed for %s.\n' "$TEST_PLATFORM"
