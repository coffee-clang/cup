#!/usr/bin/env bash

# Collects native C coverage on every supported platform and applies
# line, branch and function gates through the platform-native coverage backend.
set -euo pipefail
export LC_ALL=C LANG=C TZ=UTC

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
TESTS_ROOT="$ROOT/tests"
. "$ROOT/tests/support/common.sh"
. "$ROOT/tests/support/posix/coverage.sh"
test_begin coverage-runner
cup_quality_final_status() {
    local status

    for status in "$@"; do
        case "$status" in
            ''|*[!0-9]*) return 2 ;;
        esac
        if [ "$status" -ne 0 ]; then
            printf '%s\n' "$status"
            return 0
        fi
    done
    printf '0\n'
}
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
REPORT_DIR="${CUP_COVERAGE_DIR:-$TEST_BUILD_ROOT/reports/coverage/$PLATFORM}"
LINE_THRESHOLD="${CUP_COVERAGE_MIN_LINES:-85}"
BRANCH_THRESHOLD="${CUP_COVERAGE_MIN_BRANCHES:-70}"
FUNCTION_THRESHOLD="${CUP_COVERAGE_MIN_FUNCTIONS:-97}"
UNIT_TIMEOUT="${CUP_COVERAGE_UNIT_TIMEOUT:-1200}"
SUITE_TIMEOUT="${CUP_COVERAGE_SUITE_TIMEOUT:-300}"
REPORT_JOBS="${CUP_COVERAGE_REPORT_JOBS:-1}"
REPORT_TIMEOUT="${CUP_COVERAGE_REPORT_TIMEOUT:-600}"
HTML_TIMEOUT="${CUP_COVERAGE_HTML_TIMEOUT:-60}"

case "$PLATFORM" in
    linux-x64|linux-arm64)
        COVERAGE_BACKEND=gcov
        CC="${CC:-gcc}"
        ;;
    windows-x64)
        COVERAGE_BACKEND=gcov
        CC="${CC:-gcc}"
        ;;
    macos-x64|macos-arm64)
        COVERAGE_BACKEND=llvm
        CC="${CC:-clang}"
        ;;
    *)
        printf 'Coverage is not supported for %s.\n' "$PLATFORM" >&2
        exit 2
        ;;
esac

for value in "$LINE_THRESHOLD" "$BRANCH_THRESHOLD" "$FUNCTION_THRESHOLD"; do
    case "$value" in
        ''|*[!0-9]*)
            printf 'Coverage thresholds must be integers from 0 to 100.\n' >&2
            exit 2
            ;;
    esac
    if [ "$value" -gt 100 ]; then
        printf 'Coverage thresholds must be integers from 0 to 100.\n' >&2
        exit 2
    fi
done
for value in "$UNIT_TIMEOUT" "$SUITE_TIMEOUT" "$REPORT_JOBS" "$REPORT_TIMEOUT" "$HTML_TIMEOUT"; do
    case "$value" in
        ''|*[!0-9]*|0)
            printf 'Coverage timeouts/jobs must be positive integers.\n' >&2
            exit 2
            ;;
    esac
done

cup_test_require_tool make coverage || exit 2
if [ "$COVERAGE_BACKEND" = llvm ]; then
    cup_test_require_tool xcrun coverage || exit 2
    SDKROOT=$(xcrun --sdk macosx --show-sdk-path) || exit 2
    export SDKROOT
    CC=$(xcrun --sdk macosx --find clang) || exit 2
    LLVM_PROFDATA=$(xcrun --sdk macosx --find llvm-profdata) || exit 2
    LLVM_COV=$(xcrun --sdk macosx --find llvm-cov) || exit 2
fi
cup_test_require_tool "$CC" coverage || exit 2
if [ "$COVERAGE_BACKEND" = gcov ]; then
    compiler_id=$($CC --version 2>/dev/null | sed -n '1p')
    case "$compiler_id" in
        *GCC*|*gcc*)
            ;;
        *)
            printf 'The gcov backend requires GCC; got: %s\n' "$compiler_id" >&2
            exit 2
            ;;
    esac
    cup_test_require_tool gcov coverage || exit 2
fi
cup_test_require_tool gcovr coverage || exit 2
TIMEOUT_COMMAND=$(cup_test_find_timeout) || exit 2
if [ "$PLATFORM" = windows-x64 ]; then
    cup_test_require_tool powershell.exe 'Windows coverage integration tests' || exit 2
    cup_test_require_tool cygpath 'Windows coverage integration tests' || exit 2
fi
cup_test_require_dependencies

clean_log="$TMP_ROOT/clean.log"
build_log="$TMP_ROOT/build.log"
unit_build_log="$TMP_ROOT/unit-build.log"
helper_build_log="$TMP_ROOT/helper-build.log"
test_build_log="$TMP_ROOT/test-build.log"
cup_test_run_logged 'Cleaning previous build outputs...' "$clean_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" clean
cup_test_run_logged \
    "Building the $COVERAGE_BACKEND coverage executable..." \
    "$build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" PLATFORM="$PLATFORM" \
        CC="$CC" CUP_INTERNAL_DEPS_TARGET=deps-check coverage -j2
cup_test_run_logged \
    'Compiling instrumented unit tests...' \
    "$unit_build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" PLATFORM="$PLATFORM" \
        CC="$CC" CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=coverage test-unit-build
cup_test_run_logged \
    'Compiling instrumented test helpers...' \
    "$helper_build_log" \
    make -C "$ROOT" BUILD_DIR="$TEST_BUILD_ROOT" PLATFORM="$PLATFORM" \
        CC="$CC" CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=coverage test-helpers
cat "$unit_build_log" "$helper_build_log" >"$test_build_log"

cup_test_reset_output_directory "$REPORT_DIR"
cp "$clean_log" "$REPORT_DIR/clean.log"
cp "$build_log" "$REPORT_DIR/build.log"
cp "$test_build_log" "$REPORT_DIR/test-build.log"

{
    printf 'platform=%s\n' "$PLATFORM"
    printf 'backend=%s\n' "$COVERAGE_BACKEND"
    printf 'compiler=%s\n' "$($CC --version | sed -n '1p')"
    printf 'gcovr=%s\n' "$(gcovr --version | sed -n '1p')"
    if [ "$COVERAGE_BACKEND" = gcov ]; then
        printf 'gcov=%s\n' "$(gcov --version | sed -n '1p')"
    else
        printf 'sdkroot=%s\n' "$SDKROOT"
        printf 'llvm_profdata=%s\n' "$($LLVM_PROFDATA --version | sed -n '1p')"
        printf 'llvm_cov=%s\n' "$($LLVM_COV --version | sed -n '1p')"
    fi
    printf 'build_root=%s\n' "$TEST_BUILD_ROOT"
    printf 'unit_timeout_seconds=%s\n' "$UNIT_TIMEOUT"
    printf 'suite_timeout_seconds=%s\n' "$SUITE_TIMEOUT"
    printf 'report_timeout_seconds=%s\n' "$REPORT_TIMEOUT"
} >"$REPORT_DIR/environment.txt"

export CUP_TEST_BUILD_ROOT="$TEST_BUILD_ROOT"
export CUP_TEST_BINARY="$TEST_BUILD_ROOT/$PLATFORM/coverage/bin/cup"
[ "$PLATFORM" != windows-x64 ] || CUP_TEST_BINARY="$CUP_TEST_BINARY.exe"
if [ "$COVERAGE_BACKEND" = llvm ]; then
    mkdir -p "$REPORT_DIR/profiles"
    export LLVM_PROFILE_FILE="$REPORT_DIR/profiles/%m-%p.profraw"
fi

cup_test_run_logged 'Running instrumented C unit tests...' "$REPORT_DIR/unit.log" \
    "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$UNIT_TIMEOUT" \
        env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
        "$ROOT/tests/runners/unit.sh"

if [ "$PLATFORM" = windows-x64 ]; then
    windows_runner=$(cygpath -w "$ROOT/tests/runners/integration-windows.ps1")
    windows_binary=$(cygpath -w "$CUP_TEST_BINARY")
    windows_build_root=$(cygpath -w "$TEST_BUILD_ROOT")
    cup_test_run_logged \
        'Running instrumented Windows integration tests...' \
        "$REPORT_DIR/integration.log" \
        env CUP_TEST_BUILD_ROOT="$windows_build_root" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_runner" \
        -CupPath "$windows_binary" -Configuration coverage
else
    cup_test_run_logged \
        'Running instrumented POSIX integration tests...' \
        "$REPORT_DIR/integration.log" \
        env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
            CUP_TEST_TIMEOUT_COMMAND="$TIMEOUT_COMMAND" \
            "$ROOT/tests/runners/integration-posix.sh"
fi

if [ "$COVERAGE_BACKEND" = gcov ]; then
    printf '==> Verifying GCC test profile ownership...\n'
    cup_coverage_verify_gcov_profile_owners \
        "$TEST_BUILD_ROOT/$PLATFORM/coverage/tests" || exit 1
fi

printf '==> Waiting for coverage counters to become stable...\n'
case "$COVERAGE_BACKEND" in
    gcov)
        counter_root="$TEST_BUILD_ROOT/$PLATFORM/coverage"
        counter_pattern='*.gcda'
        ;;
    llvm)
        counter_root="$REPORT_DIR/profiles"
        counter_pattern='*.profraw'
        ;;
esac
previous=
stable=0
attempt=1
while [ "$attempt" -le 15 ]; do
    current=$(find "$counter_root" -type f -name "$counter_pattern" -exec cksum {} \; | sort | cksum)
    if [ -n "$previous" ] && [ "$current" = "$previous" ]; then
        stable=$((stable + 1))
        [ "$stable" -ge 2 ] && break
    else
        stable=0
    fi
    previous=$current
    attempt=$((attempt + 1))
    sleep 1
done
if [ "$stable" -lt 2 ]; then
    printf 'Coverage counters did not become stable.\n' >&2
    exit 1
fi

generation_status=0
threshold_status=0
html_status=0
coverage_log=$REPORT_DIR/gcovr.log

coverage_reports_complete() {
    [ -s "$REPORT_DIR/coverage.json" ] &&
    [ -s "$REPORT_DIR/coverage.xml" ] &&
    [ -s "$REPORT_DIR/coverage-summary.json" ] &&
    [ -s "$REPORT_DIR/summary.txt" ] &&
    grep -Eq '"files"[[:space:]]*:' "$REPORT_DIR/coverage.json"
}

common_args=(
    --root "$ROOT"
    --merge-mode-functions separate
    --print-summary
    --filter 'src/'
    --filter 'include/'
    --exclude 'tests/'
    --exclude 'build/'
)
backend_args=()
search_root=$counter_root

if [ "$COVERAGE_BACKEND" = llvm ]; then
    empty_profiles=$(find "$counter_root" -type f -name '*.profraw' ! -size +0c -print)
    if [ -n "$empty_profiles" ]; then
        printf 'LLVM coverage produced empty profiles:\n%s\n' "$empty_profiles" >"$coverage_log"
        generation_status=1
    fi

    llvm_binaries=("$CUP_TEST_BINARY")
    while IFS= read -r binary; do
        [ -n "$binary" ] && llvm_binaries+=("$binary")
    done < <(find "$TEST_BUILD_ROOT/$PLATFORM/coverage/tests" \
        -type f -perm -111 -size +0c | sort)

    printf '%s\n' "${llvm_binaries[@]}" >"$REPORT_DIR/llvm-binaries.txt"
    backend_args+=(--llvm-profdata-executable "$LLVM_PROFDATA")
    for binary in "${llvm_binaries[@]}"; do
        backend_args+=(--llvm-cov-binary "$binary")
    done
fi

run_gcovr() {
    jobs=$1
    "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
        gcovr -j "$jobs" "${common_args[@]}" "${backend_args[@]}" \
        --txt "$REPORT_DIR/summary.txt" \
        --xml "$REPORT_DIR/coverage.xml" --xml-pretty \
        --json "$REPORT_DIR/coverage.json" --json-pretty \
        --json-summary "$REPORT_DIR/coverage-summary.json" --json-summary-pretty \
        "$search_root"
}

if [ "$generation_status" -eq 0 ]; then
    printf '==> Generating %s coverage reports with gcovr...\n' "$COVERAGE_BACKEND"
    (cd "$ROOT" && run_gcovr "$REPORT_JOBS") >"$coverage_log" 2>&1 || generation_status=$?
fi
if [ "$generation_status" -eq 124 ] || [ "$generation_status" -eq 137 ]; then
    printf 'gcovr timed out; retrying with one worker.\n' >>"$coverage_log"
    generation_status=0
    (cd "$ROOT" && run_gcovr 1) >>"$coverage_log" 2>&1 || generation_status=$?
fi

if coverage_reports_complete && [ "$generation_status" -eq 0 ]; then
    printf '==> Validating coverage thresholds from the saved tracefile...\n' >>"$coverage_log"
    (cd "$ROOT" &&
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s \
            "$REPORT_TIMEOUT" \
        gcovr --root "$ROOT" --merge-mode-functions separate \
        --add-tracefile "$REPORT_DIR/coverage.json" --print-summary \
        --fail-under-line "$LINE_THRESHOLD" \
        --fail-under-branch "$BRANCH_THRESHOLD" \
        --fail-under-function "$FUNCTION_THRESHOLD") \
        >>"$coverage_log" 2>&1 || threshold_status=$?
else
    printf 'Coverage reports are incomplete.\n' >>"$coverage_log"
    [ "$generation_status" -ne 0 ] || generation_status=1
fi

if coverage_reports_complete; then
    printf '==> Rendering HTML coverage report...\n'
    (cd "$ROOT" &&
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s \
            "$HTML_TIMEOUT" \
        gcovr --root "$ROOT" --merge-mode-functions separate \
        --add-tracefile "$REPORT_DIR/coverage.json" \
        --html-details "$REPORT_DIR/index.html" --no-html-syntax-highlighting) \
        >"$REPORT_DIR/html.log" 2>&1 || html_status=$?
else
    printf 'Coverage reports are incomplete.\n' >"$REPORT_DIR/html.log"
fi

cat "$coverage_log"
[ ! -f "$REPORT_DIR/summary.txt" ] || cat "$REPORT_DIR/summary.txt"
{
    printf 'coverage_line_threshold=%s%%\n' "$LINE_THRESHOLD"
    printf 'coverage_branch_threshold=%s%%\n' "$BRANCH_THRESHOLD"
    printf 'coverage_function_threshold=%s%%\n' "$FUNCTION_THRESHOLD"
} >"$REPORT_DIR/thresholds.env"
final_status=$(cup_quality_final_status \
    "$generation_status" "$threshold_status" "$html_status") || exit 2
if [ "$html_status" -ne 0 ]; then
    cat "$REPORT_DIR/html.log" >&2
fi
printf 'generation_status=%s\nthreshold_status=%s\nhtml_status=%s\n' \
    "$generation_status" "$threshold_status" "$html_status" > "$REPORT_DIR/status.env"
printf 'Coverage report written to %s\n' "$REPORT_DIR"
exit "$final_status"
