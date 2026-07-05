#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
PLATFORM="${CUP_TEST_PLATFORM:-${PLATFORM:-linux-x64}}"
case "$PLATFORM" in
    linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
    *) PLATFORM=linux-x64 ;;
esac
DEPS_PREFIX="${DEPS_PREFIX:-$HOME/deps/$PLATFORM/install}"
CC="${CC:-gcc}"
TEST_BUILD_DIR="$ROOT/build/tests/unit"

mkdir -p "$TEST_BUILD_DIR"

compile_and_run() {
    name=$1
    shift
    output="$TEST_BUILD_DIR/$name"
    printf '==> Running C unit test: %s\n' "$name"
    "$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -I"$ROOT/include" -I"$DEPS_PREFIX/include" \
        "$@" \
        -L"$DEPS_PREFIX/lib" -L"$DEPS_PREFIX/lib64" -lunity -o "$output"
    "$output"
}

compile_and_run test_core \
    "$ROOT/scripts/tests/unit/test_core.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/entry.c"

compile_and_run test_info \
    "$ROOT/scripts/tests/unit/test_info.c" \
    "$ROOT/src/info.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_and_run test_checksum \
    "$ROOT/scripts/tests/unit/test_checksum.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/sha256.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c"

compile_and_run test_manifest \
    "$ROOT/scripts/tests/unit/test_manifest.c" \
    "$ROOT/src/manifest.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

printf '==> Running shell unit test: version policy\n'
"$ROOT/scripts/tests/unit/version-policy.sh"
printf '==> Running shell unit test: release metadata\n'
"$ROOT/scripts/tests/unit/release-metadata.sh"

# This test compares generated C files with the current build tree, so it is
# intentionally kept after the development build in the integration runner.
if [ "${CUP_TEST_WITH_BUILD_OUTPUT:-0}" = 1 ]; then
    printf '==> Running shell unit test: CA bundle generation\n'
    "$ROOT/scripts/tests/unit/certs.sh"
fi

printf 'All unit tests passed for %s.\n' "$PLATFORM"
