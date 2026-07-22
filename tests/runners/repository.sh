#!/usr/bin/env bash

# Purpose: Runs repository, generation and release-script contract tests.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

printf '==> Testing repository structure...\n'
"$ROOT/tests/repository/structure.sh"
printf '==> Testing source-test environment...\n'
"$ROOT/tests/repository/environment.sh"
printf '==> Testing dependency path neutralization...\n'
"$ROOT/tests/repository/dependencies.sh"
printf '==> Testing embedded CA metadata...\n'
"$ROOT/scripts/certs/check-ca-bundle.sh"
printf '==> Testing build configuration identity...\n'
"$ROOT/tests/repository/build-system.sh"
printf '==> Testing binary inspection policy...\n'
"$ROOT/tests/repository/binary-inspection.sh"
printf '==> Testing assertion quality...\n'
"$ROOT/tests/repository/assertions.sh"
printf '==> Testing coverage gap policy...\n'
"$ROOT/tests/repository/coverage-policy.sh"
printf '==> Testing version policy...\n'
"$ROOT/tests/repository/version-policy.sh"
printf '==> Testing release metadata validation...\n'
"$ROOT/tests/repository/release-metadata.sh"
printf '==> Testing filesystem and archive security...\n'
"$ROOT/tests/repository/filesystem-security.sh"
printf '==> Testing release candidate decision...\n'
"$ROOT/tests/repository/release-prepare.sh"
printf '==> Testing workflow ownership...\n'
"$ROOT/tests/repository/workflows.sh"
printf '==> Testing Tests-run provenance...\n'
"$ROOT/tests/repository/tests-run.sh"
printf '==> Testing release candidate metadata...\n'
"$ROOT/tests/repository/release-candidate.sh"
printf '==> Testing release publication recovery...\n'
"$ROOT/tests/repository/release-publish.sh"

if [ "${CUP_TEST_WITH_BUILD_OUTPUT:-0}" = 1 ]; then
    printf '==> Testing deterministic CA bundle generation...\n'
    "$ROOT/tests/repository/certs.sh"
    printf '==> Testing checkout paths containing spaces...\n'
    "$ROOT/tests/repository/build-paths.sh"
fi

printf 'All repository contract tests passed.\n'
