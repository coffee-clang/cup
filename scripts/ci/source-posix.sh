#!/bin/sh

# Executes one already-prepared native POSIX source-test plan.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"
cd "$PROJECT_ROOT"

platform=${PLATFORM:?PLATFORM is required}
family=${FAMILY:?FAMILY is required}
host_system=$(uname -s)
host_machine=$(uname -m)

fail() {
    printf 'source tests: %s\n' "$*" >&2
    exit 1
}

case "$family:$platform:$host_system:$host_machine" in
    linux:linux-x64:Linux:x86_64|linux:linux-x64:Linux:amd64) ;;
    linux:linux-arm64:Linux:aarch64|linux:linux-arm64:Linux:arm64) ;;
    macos:macos-x64:Darwin:x86_64|macos:macos-x64:Darwin:amd64) ;;
    macos:macos-arm64:Darwin:arm64|macos:macos-arm64:Darwin:aarch64) ;;
    *) fail "PLATFORM '$platform' and FAMILY '$family' do not match host $host_system/$host_machine" ;;
esac

unit_timeout=${CUP_TEST_UNIT_TIMEOUT:-300}

make PLATFORM="$platform" deps-check
PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" \
    CUP_TEST_UNIT_TIMEOUT="$unit_timeout" \
    CUP_TEST_SUITE_TIMEOUT="${CUP_TEST_SUITE_TIMEOUT:-300}" \
    make CUP_INTERNAL_DEPS_TARGET=deps-check test
make PLATFORM="$platform" CUP_INTERNAL_DEPS_TARGET=deps-check check-development
if [ "$platform" = linux-x64 ] &&
    [ "${CUP_CI_BUILD_REPOSITORY_TESTS:-0}" = 1 ]; then
    CUP_TEST_PLATFORM="$platform" CUP_TEST_BUILD_ROOT="$PROJECT_ROOT/build" \
        "$PROJECT_ROOT/tests/repository/certs.sh"
    CUP_TEST_PLATFORM="$platform" CUP_TEST_BUILD_ROOT="$PROJECT_ROOT/build" \
        "$PROJECT_ROOT/tests/repository/build-paths.sh"
    CUP_TEST_PLATFORM="$platform" \
        "$PROJECT_ROOT/tests/repository/reproducibility.sh"
fi
if [ -n "${CUP_SOURCE_EVIDENCE_ROOT:-}" ]; then
    evidence_root=$CUP_SOURCE_EVIDENCE_ROOT
else
    evidence_base=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
    case "$evidence_base" in /*) ;; *) evidence_base=$(pwd -P)/$evidence_base ;; esac
    cup_path_check_directory_chain "$evidence_base" 0 \
        'source evidence parent' || exit 1
    evidence_root=$(cup_path_create_unique_directory \
        "$evidence_base/cup-source-evidence.XXXXXX" \
        'source evidence root' 0700) ||
        fail 'could not create a unique source evidence root'
    printf 'Source evidence root: %s\n' "$evidence_root"
fi
case "$evidence_root" in /*) ;; *) evidence_root=$(pwd -P)/$evidence_root ;; esac
cup_path_prepare_directory_chain "$evidence_root" 'source evidence root' || exit 1
source_evidence=$evidence_root/$platform
source_repository=${GITHUB_REPOSITORY:-local/cup}
source_run_id=${GITHUB_RUN_ID:-1}
source_run_attempt=${GITHUB_RUN_ATTEMPT:-1}
"$PROJECT_ROOT/scripts/ci/write-source-evidence.sh" \
    "$source_evidence" "$platform" \
    "$PROJECT_ROOT/build/$platform/development/build-config.txt" \
    "$PROJECT_ROOT/build/$platform/development/generated/release.txt" \
    "$PROJECT_ROOT/build/$platform/development/binary-inspection.txt" \
    "$source_repository" "$source_run_id" "$source_run_attempt"
source_commit=$(git rev-parse HEAD)
source_artifact="cup-source-evidence-$platform-attempt-$source_run_attempt"
"$PROJECT_ROOT/scripts/ci/verify-source-evidence.sh" \
    "$source_evidence" "$platform" "$source_repository" "$source_commit" \
    "$source_run_id" "$source_run_attempt" "$source_artifact"

if [ "$platform" = linux-x64 ]; then
    make clean
    make PLATFORM="$platform" CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check
    PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" \
        CUP_TEST_UNIT_TIMEOUT="$unit_timeout" \
        make CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check test-unit
    ./scripts/build/validate-toolchain.sh "$platform" clang windres development secondary
    make PLATFORM="$platform" CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check check-development
fi

case "$platform" in
    linux-x64|linux-arm64)
        make clean
        PLATFORM="$platform" make CUP_INTERNAL_DEPS_TARGET=deps-check test-portability-linux
        ;;
esac
