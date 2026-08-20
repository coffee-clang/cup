#!/usr/bin/env bash

# Runs repository, generation and release-script quality contracts.
set -uo pipefail
export LC_ALL=C LANG=C TZ=UTC

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
cd "$ROOT" || exit 2

failures=0
REPOSITORY_TIMEOUT=${CUP_TEST_REPOSITORY_TIMEOUT:-}
TIMEOUT_COMMAND=
if [ -n "$REPOSITORY_TIMEOUT" ]; then
    case "$REPOSITORY_TIMEOUT" in
        *[!0-9]*|0)
            printf 'Invalid CUP_TEST_REPOSITORY_TIMEOUT: %s\n' \
                "$REPOSITORY_TIMEOUT" >&2
            exit 2
            ;;
    esac
    if command -v timeout >/dev/null 2>&1; then
        TIMEOUT_COMMAND=timeout
    elif command -v gtimeout >/dev/null 2>&1; then
        TIMEOUT_COMMAND=gtimeout
    else
        printf 'CUP_TEST_REPOSITORY_TIMEOUT requires GNU timeout or gtimeout.\n' >&2
        exit 2
    fi
fi

run_check() {
    local label=$1
    local script=$2

    local status=0

    printf '==> %s\n' "$label"
    if [ -n "$TIMEOUT_COMMAND" ]; then
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s \
            "$REPOSITORY_TIMEOUT" "$ROOT/$script" || status=$?
    else
        "$ROOT/$script" || status=$?
    fi
    if [ "$status" -ne 0 ]; then
        if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
            printf 'TIMED OUT after %ss: %s\n' \
                "$REPOSITORY_TIMEOUT" "$script" >&2
        else
            printf 'FAILED (status %s): %s\n' "$status" "$script" >&2
        fi
        failures=$((failures + 1))
    fi
}

run_check 'Testing repository structure...' tests/repository/structure.sh
run_check 'Testing source-test environment...' tests/repository/environment.sh
run_check 'Testing descriptor-relative path safety...' tests/repository/path-safety.sh
run_check 'Testing dependency contracts...' tests/repository/dependencies.sh
run_check 'Testing embedded CA metadata...' scripts/certs/check-ca-bundle.sh
run_check 'Testing build configuration...' tests/repository/build-system.sh
run_check 'Testing CI supply-chain policy...' tests/repository/ci-security.sh
run_check 'Testing CI evidence index...' tests/repository/ci-evidence.sh
run_check 'Testing source evidence...' tests/repository/source-evidence.sh
run_check 'Testing binary inspection policy...' tests/repository/binary-inspection.sh
run_check 'Testing version policy...' tests/repository/version-policy.sh
run_check 'Testing installer behavior and shell compatibility...' tests/repository/installer-behavior.sh
run_check 'Testing release publication recovery...' tests/repository/release-publish.sh

if [ "${CUP_TEST_WITH_BUILD_OUTPUT:-0}" = 1 ]; then
    run_check 'Testing deterministic CA bundle generation...' tests/repository/certs.sh
    run_check 'Testing checkout paths containing spaces...' tests/repository/build-paths.sh
    run_check 'Testing release artifact reproducibility...' tests/repository/reproducibility.sh
fi

if [ "$failures" -ne 0 ]; then
    printf '%s repository quality check(s) failed.\n' "$failures" >&2
    exit 1
fi
printf 'All repository quality checks passed.\n'
