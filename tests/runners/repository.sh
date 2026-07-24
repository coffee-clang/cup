#!/usr/bin/env bash

# Purpose: Runs repository, generation and release-script quality contracts.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

run_check() {
    local label=$1
    local script=$2

    printf '==> %s\n' "$label"
    "$ROOT/$script"
}

run_check 'Testing repository structure...' tests/repository/structure.sh
run_check 'Testing source-test environment...' tests/repository/environment.sh
run_check 'Testing dependency contracts...' tests/repository/dependencies.sh
run_check 'Testing embedded CA metadata...' scripts/certs/check-ca-bundle.sh
run_check 'Testing build configuration...' tests/repository/build-system.sh
run_check 'Testing binary inspection policy...' tests/repository/binary-inspection.sh
run_check 'Testing assertion quality...' tests/repository/assertions.sh
run_check 'Testing coverage policy...' tests/repository/coverage-policy.sh
run_check 'Testing version policy...' tests/repository/version-policy.sh
run_check 'Testing release metadata...' tests/repository/release-metadata.sh
run_check 'Testing filesystem and archive security...' tests/repository/filesystem-security.sh
run_check 'Testing workflow responsibilities...' tests/repository/workflows.sh
run_check 'Testing release publication recovery...' tests/repository/release-publish.sh

if [ "${CUP_TEST_WITH_BUILD_OUTPUT:-0}" = 1 ]; then
    run_check 'Testing deterministic CA bundle generation...' tests/repository/certs.sh
    run_check 'Testing checkout paths containing spaces...' tests/repository/build-paths.sh
fi

printf 'All repository quality checks passed.\n'
