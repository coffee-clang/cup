#!/bin/sh

# Purpose: Proves that a native CUP checkout can be built from a path containing
# ordinary spaces while build/dependency roots retain an explicit portable
# whitespace-free contract.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"
. "$TESTS_ROOT/support/environment.sh"

test_begin build-paths
cup_test_prepare_environment
cup_test_require_dependencies

checkout="$TMP_ROOT/native checkout with spaces/cup-main"
mkdir -p "$checkout"
(
    cd "$PROJECT_ROOT"
    tar --exclude='./.git' --exclude='./build' -cf - .
) | (
    cd "$checkout"
    tar -xf -
)

make -C "$checkout" --no-print-directory clean >/dev/null
make -C "$checkout" --no-print-directory -j2 \
    PLATFORM="$CUP_TEST_PLATFORM" DEPS_PREFIX="$DEPS_PREFIX" \
    >"$TMP_ROOT/build.log" 2>&1 || {
        cat "$TMP_ROOT/build.log" >&2
        fail 'checkout path containing spaces did not build'
    }

binary="$checkout/build/$CUP_TEST_PLATFORM/development/bin/cup"
assert_file "$binary"
"$binary" --version >"$TMP_ROOT/version.out"
assert_contains "$(cat "$TMP_ROOT/version.out")" 'cup '

config="$checkout/build/$CUP_TEST_PLATFORM/development/build-config.txt"
assert_file "$config"
escaped_checkout=$(printf '%s' "$checkout" | sed 's/ /\\ /g')
assert_contains "$(cat "$config")" "-I$escaped_checkout/include"

make -C "$checkout" --no-print-directory clean >/dev/null
[ ! -e "$checkout/build" ] || fail 'clean left checkout build output'

printf '%s\n' 'Checkout path portability tests passed.'
