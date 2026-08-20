#!/bin/sh

# Proves finalized release artifacts are independent of the selected build root.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"
. "$TESTS_ROOT/support/environment.sh"

test_begin reproducibility
cup_test_prepare_environment
cup_test_require_dependencies

case "$CUP_TEST_PLATFORM" in
    linux-*|macos-*|windows-x64)
        ;;
    *)
        fail "unsupported reproducibility test platform: $CUP_TEST_PLATFORM"
        ;;
esac

build_a=$TMP_ROOT/build-a
build_b=$TMP_ROOT/build-b

build_candidate() {
    build_root=$1
    make -C "$PROJECT_ROOT" --no-print-directory -j2 \
        PLATFORM="$CUP_TEST_PLATFORM" \
        BUILD_DIR="$build_root" \
        DEPS_PREFIX="$DEPS_PREFIX" \
        CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_BUILD_CONFIGURATION=release \
        CUP_OFFICIAL_BUILD=0 \
        _release-candidate >"$build_root.log" 2>&1 || {
        cat "$build_root.log" >&2
        fail "release reproducibility build failed: $build_root"
    }
}

build_candidate "$build_a"
build_candidate "$build_b"

final_a=$build_a/finalized/$CUP_TEST_PLATFORM/release
final_b=$build_b/finalized/$CUP_TEST_PLATFORM/release
[ -d "$final_a" ] || fail "first finalized release directory is missing: $final_a"
[ -d "$final_b" ] || fail "second finalized release directory is missing: $final_b"

case "$CUP_TEST_PLATFORM" in
    windows-x64) binary=bin/cup.exe ;;
    *) binary=bin/cup ;;
esac
[ -f "$final_a/$binary" ] || fail "first finalized artifact is missing: $binary"
[ -f "$final_b/$binary" ] || fail "second finalized artifact is missing: $binary"
cmp -s "$final_a/$binary" "$final_b/$binary" ||
    fail "finalized artifact differs across build roots: $binary"

case "$CUP_TEST_PLATFORM" in
    linux-*|windows-x64)
        symbol=symbols/cup.debug
        [ -f "$final_a/$symbol" ] || fail "first finalized symbol artifact is missing: $symbol"
        [ -f "$final_b/$symbol" ] || fail "second finalized symbol artifact is missing: $symbol"
        cmp -s "$final_a/$symbol" "$final_b/$symbol" ||
            fail "finalized symbol artifact differs across build roots: $symbol"
        ;;
    macos-*)
        symbol=symbols/cup.dSYM
        [ -d "$final_a/$symbol" ] || fail "first finalized symbol bundle is missing: $symbol"
        [ -d "$final_b/$symbol" ] || fail "second finalized symbol bundle is missing: $symbol"
        if ! diff -qr "$final_a/$symbol" "$final_b/$symbol" >"$TMP_ROOT/symbol-diff.out" 2>&1; then
            cat "$TMP_ROOT/symbol-diff.out" >&2
            fail "finalized symbol bundle differs across build roots: $symbol"
        fi
        ;;
esac

printf '%s\n' "Release binary and symbol artifacts are reproducible across build roots for $CUP_TEST_PLATFORM."
