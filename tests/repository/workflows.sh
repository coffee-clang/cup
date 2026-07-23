#!/bin/sh

# Purpose: Verifies workflow responsibilities without freezing YAML layout.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DEPENDENCIES=$ROOT/.github/workflows/dependencies.yml
TESTS=$ROOT/.github/workflows/tests.yml
RELEASE=$ROOT/.github/workflows/release.yml
DEBUG=$ROOT/.github/workflows/debug.yml
DIAGNOSTICS=$ROOT/.github/workflows/macos-coverage-diagnostics.yml

fail() {
    printf 'Workflow contract test failed: %s\n' "$*" >&2
    exit 1
}

require_text() {
    file=$1
    text=$2
    grep -Fq -- "$text" "$file" || fail "$file is missing: $text"
}

reject_text() {
    file=$1
    text=$2
    ! grep -Fq -- "$text" "$file" || fail "$file contains forbidden contract: $text"
}

for required in 'workflow_call:' 'workflow_dispatch:' 'actions/cache@v5' \
        '--print-cache-key' 'make PLATFORM=' ' deps' ' deps-check' \
        'linux-x64-gcc' 'linux-arm64-gcc' 'macos-x64-apple-clang' \
        'macos-arm64-apple-clang' 'windows-x64-ucrt64' \
        'windows-x64-clang64'; do
    require_text "$DEPENDENCIES" "$required"
done

for required in 'workflow_call:' 'uses: ./.github/workflows/dependencies.yml' \
        'run: make quality' 'Source tests' 'Coverage' 'Sanitizers' \
        'Tests gate'; do
    require_text "$TESTS" "$required"
done

# Dependency consumers must restore exactly the path used by the producer and
# pass that resolved prefix to every build or test entry point.
for consumer in "$TESTS" "$RELEASE" "$DEBUG" "$DIAGNOSTICS"; do
    require_text "$consumer" 'fail-on-cache-miss: true'
    require_text "$consumer" 'DEPS_PREFIX:'
    reject_text "$consumer" 'path: ~/deps/'
done
require_text "$TESTS" 'path: ${{ steps.cache.outputs.prefix }}'
require_text "$TESTS" 'path: ${{ steps.cache-posix.outputs.prefix }}'
require_text "$TESTS" 'DEPS_PREFIX: ${{ steps.cache-windows.outputs.prefix }}'
reject_text "$TESTS" 'source-windows.ps1'
[ ! -e "$ROOT/scripts/ci/source-windows.ps1" ] || \
    fail 'obsolete PowerShell source-test wrapper still exists'
reject_text "$TESTS" 'scripts/release/'
reject_text "$TESTS" 'gh release'

for required in 'workflow_dispatch:' 'uses: ./.github/workflows/tests.yml' \
        'build-release:' 'native-release-tests:' 'scripts/release/publish.sh' \
        'pattern: cup-release-*' 'PUBLIC_RELEASE_TOKEN'; do
    require_text "$RELEASE" "$required"
done
# Release candidates must come from the current run; cross-run artifact
# downloads would require a run-id selector and are intentionally forbidden.
reject_text "$RELEASE" 'run-id:'

for required in 'workflow_dispatch:' 'push:' '- main' \
        'uses: ./.github/workflows/dependencies.yml'; do
    require_text "$DEBUG" "$required"
done

for required in 'workflow_dispatch:' 'diagnostics/macos-coverage' \
        'uses: ./.github/workflows/dependencies.yml' \
        'scripts/ci/macos-coverage-diagnostics.sh' 'actions/upload-artifact@v7'; do
    require_text "$DIAGNOSTICS" "$required"
done
reject_text "$DIAGNOSTICS" 'CUP_COVERAGE_MIN_'

reject_text "$DEPENDENCIES" 'push:'
require_text "$DEPENDENCIES" 'group: cup-dependencies-'
require_text "$DEPENDENCIES" 'cancel-in-progress: false'
if grep -R -Fq 'C:/msys64/home/runneradmin' "$ROOT/.github/workflows"; then
    fail 'workflow cache paths must not depend on a runner account name'
fi

printf '%s\n' 'Workflow responsibility tests passed.'
