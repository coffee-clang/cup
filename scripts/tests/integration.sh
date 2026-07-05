#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

run_suite() {
    label=$1
    script=$2
    printf '==> %s\n' "$label"
    "$ROOT/scripts/tests/integration/$script"
}

if [ "${CUP_TEST_SKIP_BUILD:-0}" = 1 ]; then
    TEST_SCRIPT_DIR="$ROOT/scripts/tests/integration"
    . "$ROOT/scripts/tests/support/common.sh"
    require_test_binary
else
    run_suite "Building development binaries for ${CUP_TEST_PLATFORM:-current platform}..." \
        build.sh
fi

if [ "${CUP_TEST_SKIP_BUILD:-0}" != 1 ]; then
    run_suite 'Testing generated CA bundle...' ../unit/certs.sh
fi

run_suite 'Testing manifest checksum schema...' manifest.sh
run_suite 'Testing public command behavior...' commands.sh
run_suite 'Testing state persistence...' state.sh
run_suite 'Testing managed entry points...' entrypoints.sh
run_suite 'Testing transaction recovery...' recovery.sh
run_suite 'Testing detached uninstall cleanup...' uninstall.sh

printf 'All POSIX integration tests passed.\n'
