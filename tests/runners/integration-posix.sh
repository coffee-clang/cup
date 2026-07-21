#!/bin/sh

# Purpose: Runs all POSIX real-CLI workflow owners against a prepared or freshly built executable.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment

run_suite() {
    label=$1
    script=$2
    printf '==> %s\n' "$label"

    if [ -n "${CUP_TEST_SUITE_TIMEOUT:-}" ]; then
        case "$CUP_TEST_SUITE_TIMEOUT" in
            *[!0-9]*|'')
                printf 'Invalid CUP_TEST_SUITE_TIMEOUT: %s\n' \
                    "$CUP_TEST_SUITE_TIMEOUT" >&2
                exit 2
                ;;
        esac
        command -v timeout >/dev/null 2>&1 || {
            printf 'timeout is required when CUP_TEST_SUITE_TIMEOUT is set.\n' >&2
            exit 2
        }
        timeout --foreground --signal=TERM --kill-after=30s \
            "$CUP_TEST_SUITE_TIMEOUT" \
            "$ROOT/tests/integration/posix/$script"
    else
        "$ROOT/tests/integration/posix/$script"
    fi
}

if [ "${CUP_TEST_SKIP_BUILD:-0}" = 1 ]; then
    TESTS_ROOT="$ROOT/tests"
    export TESTS_ROOT
    . "$TESTS_ROOT/support/posix-cli.sh"
    require_test_binary
else
    cup_test_require_dependencies
    run_suite "Building development binaries for $CUP_TEST_PLATFORM..." \
        build.sh
fi

run_suite 'Testing package catalog checksum schema...' package-catalog.sh
run_suite 'Testing unsafe package archives...' archive-safety.sh
run_suite 'Testing CLI parsing and dispatch contracts...' cli-contract.sh
run_suite 'Testing the public package lifecycle...' package-lifecycle.sh
run_suite 'Testing install preferences, profiles and toolchains...' install-policy.sh
run_suite 'Testing state persistence...' state.sh
run_suite 'Testing managed wrappers...' wrappers.sh
run_suite 'Testing transaction recovery...' recovery.sh
run_suite 'Testing deterministic repair...' repair.sh
run_suite 'Testing read-only diagnostics...' doctor.sh
run_suite 'Testing concurrent commands...' concurrency.sh
run_suite 'Testing detached uninstall cleanup...' uninstall.sh

printf 'All POSIX integration tests passed.\n'
