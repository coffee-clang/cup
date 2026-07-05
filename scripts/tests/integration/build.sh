#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

build_with_compiler() {
    compiler=$1
    make -C "$PROJECT_ROOT" clean >/dev/null
    make -C "$PROJECT_ROOT" PLATFORM="$TEST_PLATFORM" LINK_MODE=dynamic \
        BUILD_MODE=development CC="$compiler" -j2 >/dev/null
    output=$("$TEST_BINARY" --version)
    assert_contains "$output" 'cup 0.2.0-dev'
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
