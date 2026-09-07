#!/bin/sh

# Executes one already-prepared native POSIX source-test plan.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"
# shellcheck source=../lib/platform-domain.sh
. "$PROJECT_ROOT/scripts/lib/platform-domain.sh"
cd "$PROJECT_ROOT"

platform=${PLATFORM:?PLATFORM is required}
family=${FAMILY:?FAMILY is required}
host_system=$(uname -s)
host_machine=$(uname -m)

fail() {
    printf 'source tests: %s\n' "$*" >&2
    exit 1
}

native_platform=$(cup_platform_from_uname "$host_system" "$host_machine") ||
    fail "unsupported native host $host_system/$host_machine"
[ "$platform" = "$native_platform" ] ||
    fail "PLATFORM '$platform' does not match host $host_system/$host_machine"
case "$family:$platform" in
    linux:linux-x64|linux:linux-arm64|macos:macos-x64|macos:macos-arm64) ;;
    *) fail "PLATFORM '$platform' and FAMILY '$family' do not match" ;;
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
if [ -n "${CUP_SOURCE_BUILD_CONFIG:-}" ]; then
    source_build_config=$CUP_SOURCE_BUILD_CONFIG
    case "$source_build_config" in
        /*) ;;
        *) source_build_config=$(pwd -P)/$source_build_config ;;
    esac
    cup_path_prepare_file_target "$source_build_config" 'source-tested build config' || exit 1
    cup_path_copy_file \
        "$PROJECT_ROOT/build/$platform/development/build-config.txt" \
        "$source_build_config" 0644 replace ||
        fail 'could not preserve the primary source-tested build config'
fi

if [ "$platform" = linux-x64 ]; then
    make clean
    make PLATFORM="$platform" CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_INTERNAL_TOOLCHAIN_ROLE=secondary
    PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" \
        CUP_TEST_UNIT_TIMEOUT="$unit_timeout" \
        make CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check \
            CUP_INTERNAL_TOOLCHAIN_ROLE=secondary test-unit
    ./scripts/build/validate-toolchain.sh "$platform" clang windres development secondary
    make PLATFORM="$platform" CC=clang CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_INTERNAL_TOOLCHAIN_ROLE=secondary check-development
fi

case "$platform" in
    linux-x64|linux-arm64)
        make clean
        PLATFORM="$platform" make CUP_INTERNAL_DEPS_TARGET=deps-check test-portability-linux
        ;;
esac
