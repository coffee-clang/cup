#!/bin/sh

# Purpose: Verifies workflow responsibilities without freezing YAML layout.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DEPENDENCIES=$ROOT/.github/workflows/dependencies.yml
TESTS=$ROOT/.github/workflows/tests.yml
RELEASE=$ROOT/.github/workflows/release.yml
DEBUG=$ROOT/.github/workflows/debug.yml

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

require_text "$DEBUG" 'uses: ./.github/workflows/dependencies.yml'

reject_text "$DEPENDENCIES" 'push:'
require_text "$DEPENDENCIES" 'group: cup-dependencies-'
require_text "$DEPENDENCIES" 'cancel-in-progress: false'
reject_text "$DEBUG" 'push:'
if grep -R -Fq 'C:/msys64/home/runneradmin' "$ROOT/.github/workflows"; then
    fail 'workflow cache paths must not depend on a runner account name'
fi

printf '%s\n' 'Workflow responsibility tests passed.'
