#!/usr/bin/env bash

# Runs ASan/UBSan on every supported native platform, with leak
# detection enabled only where the runtime supports it.
set -euo pipefail
export LC_ALL=C LANG=C TZ=UTC

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
TESTS_ROOT="$ROOT/tests"
. "$ROOT/tests/support/common.sh"
test_begin sanitizer-runner
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
REPORT_DIR="${CUP_SANITIZER_DIR:-$TEST_BUILD_ROOT/reports/sanitizers/$PLATFORM}"
UNIT_TIMEOUT="${CUP_SANITIZER_UNIT_TIMEOUT:-1200}"
SUITE_TIMEOUT="${CUP_SANITIZER_SUITE_TIMEOUT:-300}"

case "$PLATFORM" in
    linux-x64|linux-arm64)
        CC="${CC:-clang}"
        LEAKS=1
        ;;
    macos-x64|macos-arm64)
        CC="${CC:-clang}"
        LEAKS=0
        ;;
    windows-x64)
        CC="${CC:-clang}"
        LEAKS=0
        ;;
    *)
        printf 'Sanitizers are not supported for %s.\n' "$PLATFORM" >&2
        exit 2
        ;;
esac

cup_test_require_tool make sanitizers || exit 2
cup_test_require_tool "$CC" sanitizers || exit 2
for value in "$UNIT_TIMEOUT" "$SUITE_TIMEOUT"; do
    case "$value" in
        ''|*[!0-9]*|0)
            printf 'Sanitizer timeouts must be positive integers.\n' >&2
            exit 2
            ;;
    esac
done
TIMEOUT_COMMAND=$(cup_test_find_timeout) || exit 2
if [ "$PLATFORM" = windows-x64 ]; then
    if [ "${MSYSTEM:-}" != CLANG64 ] || [ "${MINGW_PREFIX:-}" != /clang64 ]; then
        printf '%s\n' \
            'Windows sanitizer tests require the isolated MSYS2 CLANG64 environment.' >&2
        exit 2
    fi
    cup_test_require_tool llvm-windres sanitizers || exit 2
    cup_test_require_tool powershell.exe 'Windows sanitizer integration tests' || exit 2
    cup_test_require_tool cygpath 'Windows sanitizer integration tests' || exit 2
fi
cup_test_require_dependencies

MAKE_PLATFORM_ARGS=(PLATFORM="$PLATFORM" CC="$CC")
if [ "$PLATFORM" = windows-x64 ]; then
    MAKE_PLATFORM_ARGS+=(WINDRES=llvm-windres)
fi

smoke_dir="$TMP_ROOT/smoke"
mkdir "$smoke_dir"
cat > "$smoke_dir/smoke.c" <<'EOF'
#include <stdlib.h>
int main(void) { void *p = malloc(1); free(p); return p == NULL; }
EOF
if ! "$CC" -fsanitize=address,undefined "$smoke_dir/smoke.c" -o "$smoke_dir/smoke" \
    >"$smoke_dir/build.log" 2>&1 || \
        ! "$smoke_dir/smoke" >"$smoke_dir/run.log" 2>&1; then
    [ ! -f "$smoke_dir/build.log" ] || cat "$smoke_dir/build.log" >&2
    [ ! -f "$smoke_dir/run.log" ] || cat "$smoke_dir/run.log" >&2
    printf 'The selected compiler does not provide working ASan/UBSan runtimes.\n' >&2
    exit 2
fi

clean_log="$TMP_ROOT/clean.log"
build_log="$TMP_ROOT/build.log"
unit_build_log="$TMP_ROOT/unit-build.log"
helper_build_log="$TMP_ROOT/helper-build.log"
test_build_log="$TMP_ROOT/test-build.log"
cup_test_run_logged 'Cleaning previous build outputs...' "$clean_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" clean
cup_test_run_logged 'Building the sanitizer executable...' "$build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" "${MAKE_PLATFORM_ARGS[@]}" \
        CUP_INTERNAL_DEPS_TARGET=deps-check sanitizers -j2
cup_test_run_logged 'Compiling sanitizer unit tests...' \
    "$unit_build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" "${MAKE_PLATFORM_ARGS[@]}" \
        CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=sanitizers test-unit-build
cup_test_run_logged 'Compiling sanitizer test helpers...' \
    "$helper_build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" "${MAKE_PLATFORM_ARGS[@]}" \
        CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=sanitizers test-helpers
cat "$unit_build_log" "$helper_build_log" >"$test_build_log"

cup_test_reset_output_directory "$REPORT_DIR"
cp "$clean_log" "$REPORT_DIR/clean.log"
cp "$build_log" "$REPORT_DIR/build.log"
cp "$test_build_log" "$REPORT_DIR/test-build.log"
ASAN_BASE_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
UNIT_ASAN_OPTIONS="$ASAN_BASE_OPTIONS:detect_leaks=$LEAKS"
INTEGRATION_ASAN_OPTIONS="$ASAN_BASE_OPTIONS:detect_leaks=$LEAKS"
UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1"
export UBSAN_OPTIONS
if [ "$LEAKS" = 1 ]; then
    LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}exitcode=23"
    export LSAN_OPTIONS
fi
if symbolizer=$(cup_test_find_llvm_tool llvm-symbolizer 2>/dev/null); then
    export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-$symbolizer}"
fi

{
    printf 'platform=%s\n' "$PLATFORM"
    printf 'compiler=%s\n' "$($CC --version | sed -n '1p')"
    printf 'unit_asan_options=%s\n' "$UNIT_ASAN_OPTIONS"
    printf 'integration_asan_options=%s\n' "$INTEGRATION_ASAN_OPTIONS"
    printf 'ubsan_options=%s\n' "$UBSAN_OPTIONS"
    printf 'unit_leak_detection=%s\n' "$LEAKS"
    printf 'integration_leak_detection=%s\n' "$LEAKS"
    printf 'build_root=%s\n' "$TEST_BUILD_ROOT"
    printf 'unit_timeout_seconds=%s\n' "$UNIT_TIMEOUT"
    printf 'suite_timeout_seconds=%s\n' "$SUITE_TIMEOUT"
} >"$REPORT_DIR/environment.txt"

export CUP_TEST_BUILD_ROOT="$TEST_BUILD_ROOT"
export CUP_TEST_BINARY="$TEST_BUILD_ROOT/$PLATFORM/sanitizers/bin/cup"
[ "$PLATFORM" != windows-x64 ] || CUP_TEST_BINARY="$CUP_TEST_BINARY.exe"
cup_test_run_logged 'Running sanitizer unit tests...' "$REPORT_DIR/unit.log" \
    "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$UNIT_TIMEOUT" \
        env ASAN_OPTIONS="$UNIT_ASAN_OPTIONS" \
            CUP_TEST_CONFIGURATION=sanitizers CUP_TEST_PLATFORM="$PLATFORM" \
            CUP_TEST_BUILD_ROOT="$TEST_BUILD_ROOT" \
            "$ROOT/tests/runners/unit.sh"

if [ "$PLATFORM" = windows-x64 ]; then
    windows_runner=$(cygpath -w "$ROOT/tests/runners/integration-windows.ps1")
    windows_binary=$(cygpath -w "$CUP_TEST_BINARY")
    windows_build_root=$(cygpath -w "$TEST_BUILD_ROOT")
    cup_test_run_logged \
        'Running sanitizer Windows integration tests...' \
        "$REPORT_DIR/integration.log" \
        env ASAN_OPTIONS="$INTEGRATION_ASAN_OPTIONS" \
            CUP_TEST_BUILD_ROOT="$windows_build_root" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_runner" \
            -CupPath "$windows_binary" -Configuration sanitizers
else
    cup_test_run_logged \
        'Running sanitizer POSIX integration tests...' \
        "$REPORT_DIR/integration.log" \
        env ASAN_OPTIONS="$INTEGRATION_ASAN_OPTIONS" \
            CUP_TEST_CONFIGURATION=sanitizers CUP_TEST_PLATFORM="$PLATFORM" \
            CUP_TEST_BUILD_ROOT="$TEST_BUILD_ROOT" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
            CUP_TEST_TIMEOUT_COMMAND="$TIMEOUT_COMMAND" \
            "$ROOT/tests/runners/integration-posix.sh"
fi
printf 'Sanitizer tests passed for %s. Logs: %s\n' "$PLATFORM" "$REPORT_DIR"
