#!/usr/bin/env bash

# Purpose: Runs ASan/UBSan on every supported native platform, with leak
# leak detection enabled only where the runtime supports it.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"
REPORT_DIR="${CUP_SANITIZER_DIR:-$ROOT/build/sanitizers/$PLATFORM}"
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

smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/cup-sanitizer-smoke.XXXXXX")
trap 'rm -rf "$smoke_dir"' EXIT HUP INT TERM
cat > "$smoke_dir/smoke.c" <<'EOF'
#include <stdlib.h>
int main(void) { void *p = malloc(1); free(p); return p == NULL; }
EOF
if ! "$CC" -fsanitize=address,undefined "$smoke_dir/smoke.c" -o "$smoke_dir/smoke" \
    >"$smoke_dir/build.log" 2>&1 || ! "$smoke_dir/smoke" >"$smoke_dir/run.log" 2>&1; then
    cat "$smoke_dir/build.log" "$smoke_dir/run.log" >&2
    printf 'The selected compiler does not provide working ASan/UBSan runtimes.\n' >&2
    exit 2
fi
rm -rf "$smoke_dir"
trap - EXIT HUP INT TERM

clean_log=$(mktemp "${TMPDIR:-/tmp}/cup-sanitizers-clean.XXXXXX")
trap 'rm -f "$clean_log"' EXIT HUP INT TERM
if ! make -C "$ROOT" clean >"$clean_log" 2>&1; then
    cat "$clean_log" >&2
    exit 1
fi
mkdir -p "$REPORT_DIR"
rm -rf "$REPORT_DIR"/*
cp "$clean_log" "$REPORT_DIR/clean.log"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:detect_leaks=$LEAKS}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if [ "$LEAKS" = 1 ]; then
    export LSAN_OPTIONS="${LSAN_OPTIONS:-exitcode=23}"
fi
if symbolizer=$(cup_test_find_llvm_tool llvm-symbolizer 2>/dev/null); then
    export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-$symbolizer}"
fi

{
    printf 'platform=%s\n' "$PLATFORM"
    printf 'compiler=%s\n' "$($CC --version | sed -n '1p')"
    printf 'asan_options=%s\n' "$ASAN_OPTIONS"
    printf 'ubsan_options=%s\n' "$UBSAN_OPTIONS"
    printf 'leak_detection=%s\n' "$LEAKS"
    printf 'unit_timeout_seconds=%s\n' "$UNIT_TIMEOUT"
    printf 'suite_timeout_seconds=%s\n' "$SUITE_TIMEOUT"
} >"$REPORT_DIR/environment.txt"

run_logged() {
    label=$1
    log_file=$2
    shift 2
    printf '==> %s\n' "$label"
    if ! "$@" >"$log_file" 2>&1; then
        cat "$log_file" >&2
        return 1
    fi
}

run_logged 'Building the sanitizer executable...' "$REPORT_DIR/build.log" \
    make -C "$ROOT" "${MAKE_PLATFORM_ARGS[@]}" sanitizers -j2
run_logged 'Compiling sanitizer unit tests and helpers...' "$REPORT_DIR/test-build.log" \
    make -C "$ROOT" "${MAKE_PLATFORM_ARGS[@]}" \
        CUP_TEST_CONFIGURATION=sanitizers test-unit-build test-helpers

export CUP_TEST_BINARY="$ROOT/build/$PLATFORM/sanitizers/bin/cup"
[ "$PLATFORM" != windows-x64 ] || CUP_TEST_BINARY="$CUP_TEST_BINARY.exe"
run_logged 'Running sanitizer unit tests...' "$REPORT_DIR/unit.log" \
    "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$UNIT_TIMEOUT" \
        env CUP_TEST_CONFIGURATION=sanitizers CUP_TEST_PLATFORM="$PLATFORM" \
            "$ROOT/tests/runners/unit.sh"

if [ "$PLATFORM" = windows-x64 ]; then
    windows_runner=$(cygpath -w "$ROOT/tests/integration/windows/run.ps1")
    windows_binary=$(cygpath -w "$CUP_TEST_BINARY")
    run_logged 'Running sanitizer Windows integration tests...' "$REPORT_DIR/integration.log" \
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$SUITE_TIMEOUT" \
            powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_runner" \
            -CupPath "$windows_binary"
else
    run_logged 'Running sanitizer POSIX integration tests...' "$REPORT_DIR/integration.log" \
        env CUP_TEST_CONFIGURATION=sanitizers CUP_TEST_PLATFORM="$PLATFORM" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
            CUP_TEST_TIMEOUT_COMMAND="$TIMEOUT_COMMAND" \
            "$ROOT/tests/runners/integration-posix.sh"
fi
printf 'Sanitizer tests passed for %s. Logs: %s\n' "$PLATFORM" "$REPORT_DIR"
