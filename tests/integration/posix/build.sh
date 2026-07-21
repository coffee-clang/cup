#!/bin/sh

# Purpose: Builds the development executable used by the POSIX integration
# owner when no prepared binary is supplied.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

build_with_compiler() {
    compiler=$1
    base_version=$(sed -n '1p' "$PROJECT_ROOT/VERSION" | tr -d '\r')
    make -C "$PROJECT_ROOT" clean >/dev/null
    make -C "$PROJECT_ROOT" PLATFORM="$TEST_PLATFORM" CC="$compiler" -j2 >/dev/null
    output=$("$TEST_BINARY" --version)
    assert_contains "$output" "cup $base_version-dev"
}

case "$TEST_PLATFORM" in
    linux-*)
        build_with_compiler gcc
        build_with_compiler clang
        ;;
    macos-*)
        build_with_compiler clang
        ;;
    *)
        fail "unsupported POSIX build test platform: $TEST_PLATFORM"
        ;;
esac

require_test_binary
printf 'Development build tests passed for %s.\n' "$TEST_PLATFORM"
