#!/usr/bin/env bash

# Purpose: Locks test/release ownership, provenance gates and debug artifacts.
set -euo pipefail

TESTS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$TESTS_ROOT/support/common.sh"
ROOT="$PROJECT_ROOT"
test_begin workflows

workflow_dir="$ROOT/.github/workflows"
actual=$(find "$workflow_dir" -maxdepth 1 -type f -name '*.yml' -printf '%f\n' | sort)
expected=$(printf '%s\n' debug.yml release.yml static.yml tests.yml)
[ "$actual" = "$expected" ] || fail "unexpected workflow set: $actual"

tests_workflow="$workflow_dir/tests.yml"
release_workflow="$workflow_dir/release.yml"
debug_workflow="$workflow_dir/debug.yml"

workflow_pushes_main() {
    awk '
        /^  push:$/ { in_push = 1; next }
        in_push && /^  [[:alnum:]_-]+:/ { exit }
        in_push && /^      - main$/ { found = 1 }
        END { exit found ? 0 : 1 }
    ' "$1"
}

# Tests workflow: source ownership, native matrices and candidate gates.
grep -Fxq 'name: Tests' "$tests_workflow" || fail 'tests.yml must be named Tests'
grep -Fq 'workflow_dispatch:' "$tests_workflow" || fail 'tests.yml must support workflow_dispatch'
workflow_pushes_main "$tests_workflow" || fail 'tests.yml push trigger must target main'
for job in prepare posix windows coverage sanitizers common-assets build-release native-release-tests gate; do
    grep -Eq "^  $job:" "$tests_workflow" || fail "tests.yml is missing job $job"
done
for platform in linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64; do
    grep -Fq "$platform" "$tests_workflow" || fail "tests.yml is missing supported platform $platform"
done
for required in \
    'tests/runners/coverage.sh' 'tests/runners/sanitizers.sh' \
    'tests/release/posix.sh' 'tests/release/windows.ps1' \
    'make PLATFORM=linux-x64 deps' 'JOBS=4 make PLATFORM=windows-x64 deps' \
    'msystem: UCRT64' 'pattern: cup-release-*' \
    'CUP_COVERAGE_MIN_LINES: 85' 'CUP_COVERAGE_MIN_BRANCHES: 70' \
    'CUP_COVERAGE_MIN_FUNCTIONS: 97' \
    'github.token' 'RELEASE_REPOSITORY: coffee-clang/cup' \
    'SOURCE_REPOSITORY: ${{ github.repository }}' \
    'TESTS_RUN_ID: ${{ github.run_id }}' \
    'TESTS_RUN_ATTEMPT: ${{ github.run_attempt }}'; do
    grep -Fq "$required" "$tests_workflow" || fail "tests.yml is missing required gate: $required"
done
for forbidden in source-windows-deps.sh 'curl:p' 'libarchive:p' 'argtable3:p' 'DEPS_PREFIX=/ucrt64' CUP_DEPS_SCOPE; do
    ! grep -Fq "$forbidden" "$tests_workflow" || fail "tests.yml contains forbidden dependency path: $forbidden"
done
awk '
    /^          - platform: windows-x64$/ { windows = 1; next }
    windows && /^            runner: windows-latest$/ { found = 1; exit }
    windows && /^          - platform:/ { windows = 0 }
    END { exit found ? 0 : 1 }
' "$tests_workflow" || fail 'Windows release candidate must be built natively on windows-latest'

# Release workflow: publish only a successful Tests run for the same commit.
grep -Fxq 'name: Release' "$release_workflow" || fail 'release.yml must be named Release'
grep -Fq 'workflow_dispatch:' "$release_workflow" || fail 'release.yml must be manual'
for required in 'tests_run_id' 'scripts/release/resolve-tests-run.sh' \
    'run-id: ${{ steps.tests.outputs.run_id }}' \
    'EXPECTED_SHA: ${{ steps.metadata.outputs.sha }}' \
    'secrets.PUBLIC_RELEASE_TOKEN' 'GH_REPO: coffee-clang/cup'; do
    grep -Fq "$required" "$release_workflow" || fail "release.yml is missing required provenance rule: $required"
done
for forbidden in 'make ' 'scripts/release/build-platform.sh' 'workflow_run:'; do
    ! grep -Fq "$forbidden" "$release_workflow" || fail "release.yml performs work owned by Tests: $forbidden"
done

# Debug workflow: diagnostics only, without official identity or publication credentials.
grep -Fxq 'name: Debug artifacts' "$debug_workflow" || fail 'debug.yml must be named Debug artifacts'
grep -Fq 'workflow_dispatch:' "$debug_workflow" || fail 'debug.yml must support workflow_dispatch'
workflow_pushes_main "$debug_workflow" || fail 'debug.yml push trigger must target main'
for platform in linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64; do
    grep -Fq "$platform" "$debug_workflow" || fail "debug.yml is missing supported platform $platform"
done
for required in 'make PLATFORM=${{ matrix.platform }} debug' 'configuration=debug' \
    'objcopy --only-keep-debug' dsymutil 'actions/upload-artifact' \
    'rebuild-commands.txt' 'archive: false' \
    'cup-debug-${{ matrix.platform }}.tar.gz' 'binary-inspection.txt' \
    'JOBS=4 make PLATFORM=windows-x64 deps' 'msystem: UCRT64'; do
    grep -Fq "$required" "$debug_workflow" || fail "debug.yml is missing diagnostic output: $required"
done
for forbidden in 'scripts/release/publish.sh' 'gh release' 'CUP_OFFICIAL_BUILD=1' \
    source-windows-deps.sh 'curl:p' 'libarchive:p' 'argtable3:p' 'DEPS_PREFIX=/ucrt64'; do
    ! grep -Fq "$forbidden" "$debug_workflow" || fail "debug.yml publishes or bypasses pinning: $forbidden"
done

# CI entry points must delegate dependency preparation and binary inspection to Make.
source_posix="$ROOT/scripts/ci/source-posix.sh"
[ "$(grep -Fc 'make PLATFORM="$platform" deps' "$source_posix")" -eq 2 ] ||
    fail 'POSIX CI must prepare both Linux and macOS through make deps'
if grep -Eq 'scripts/dependencies/build-(posix|windows)\.sh' "$source_posix"; then
    fail 'POSIX CI bypasses the canonical make deps entry point'
fi

if PLATFORM=macos-x64 FAMILY=linux "$source_posix" \
        >"$TMP_ROOT/source-platform-mismatch.out" 2>&1; then
    fail 'POSIX source CI accepted a family/platform mismatch'
fi
assert_contains "$(cat "$TMP_ROOT/source-platform-mismatch.out")" 'do not match host'

if PLATFORM=macos-x64 FAMILY=linux "$ROOT/scripts/release/build-platform.sh" \
        >"$TMP_ROOT/release-platform-mismatch.out" 2>&1; then
    fail 'release build accepted a family/platform mismatch'
fi
assert_contains "$(cat "$TMP_ROOT/release-platform-mismatch.out")" 'do not match host'

grep -Fq 'make PLATFORM="$platform" check-binary' "$source_posix" ||
    fail 'POSIX source CI does not inspect the linked executable'
grep -Fq 'check-binary' "$ROOT/scripts/ci/source-windows.ps1" ||
    fail 'Windows source CI does not inspect the linked executable'
grep -Fq 'CUP_BUILD_CONFIGURATION=release check-binary' "$ROOT/scripts/release/build-platform.sh" ||
    fail 'release construction does not inspect every candidate executable'

grep -Fq 'PKG_CONFIG_LIBDIR=$(STATIC_PKG_CONFIG_PATH)' "$ROOT/Makefile" ||
    fail 'static linking can fall back to host pkg-config metadata'
grep -Fq 'PKG_CONFIG_SYSROOT_DIR=' "$ROOT/Makefile" ||
    fail 'static linking can inherit a host pkg-config sysroot'

printf 'Workflow ownership and release provenance tests passed.\n'
