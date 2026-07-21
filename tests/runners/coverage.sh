#!/usr/bin/env bash

# Purpose: Builds GCC-instrumented product/tests, runs them, and applies gcovr gates.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"
REPORT_DIR="${CUP_COVERAGE_DIR:-$ROOT/build/coverage}"
LINE_THRESHOLD="${CUP_COVERAGE_MIN_LINES:-85}"
BRANCH_THRESHOLD="${CUP_COVERAGE_MIN_BRANCHES:-70}"
FUNCTION_THRESHOLD="${CUP_COVERAGE_MIN_FUNCTIONS:-97}"
UNIT_TIMEOUT="${CUP_COVERAGE_UNIT_TIMEOUT:-1200}"
SUITE_TIMEOUT="${CUP_COVERAGE_SUITE_TIMEOUT:-300}"
REPORT_JOBS="${CUP_COVERAGE_REPORT_JOBS:-1}"
REPORT_TIMEOUT="${CUP_COVERAGE_REPORT_TIMEOUT:-120}"
HTML_TIMEOUT="${CUP_COVERAGE_HTML_TIMEOUT:-30}"

# Validate platform, tools, numeric gates and timeout controls before compiling anything.
case "$PLATFORM" in
    linux-x64|linux-arm64) ;;
    *) printf 'Coverage is supported only for Linux POSIX builds.\n' >&2; exit 2 ;;
esac
for command in gcov gcovr timeout; do
    command -v "$command" >/dev/null 2>&1 || {
        printf '%s is required for coverage collection.\n' "$command" >&2
        exit 2
    }
done
for value in "$LINE_THRESHOLD" "$BRANCH_THRESHOLD" "$FUNCTION_THRESHOLD"; do
    case "$value" in
        '' | *[!0-9]*)
            printf 'Coverage thresholds must be non-negative integers.\n' >&2
            exit 2
            ;;
    esac
done
for value in "$UNIT_TIMEOUT" "$SUITE_TIMEOUT" "$REPORT_JOBS" "$REPORT_TIMEOUT" "$HTML_TIMEOUT"; do
    case "$value" in
        '' | *[!0-9]* | 0)
            printf 'Coverage timeouts/jobs must be positive integers.\n' >&2
            exit 2
            ;;
    esac
done
cup_test_require_dependencies

# Run each existing owner with bounded logs and preserve its full diagnostic output.
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

clean_log=$(mktemp "${TMPDIR:-/tmp}/cup-coverage-clean.XXXXXX")
trap 'rm -f "$clean_log"' EXIT HUP INT TERM
run_logged 'Cleaning previous build outputs...' "$clean_log" make -C "$ROOT" clean
mkdir -p "$REPORT_DIR"
rm -rf "$REPORT_DIR"/*
cp "$clean_log" "$REPORT_DIR/clean.log"

{
    printf 'platform=%s\n' "$PLATFORM"
    printf 'compiler=%s\n' "$(gcc --version | sed -n '1p')"
    printf 'gcov=%s\n' "$(gcov --version | sed -n '1p')"
    printf 'gcovr=%s\n' "$(gcovr --version | sed -n '1p')"
    printf 'unit_timeout_seconds=%s\n' "$UNIT_TIMEOUT"
    printf 'suite_timeout_seconds=%s\n' "$SUITE_TIMEOUT"
    printf 'report_timeout_seconds=%s\n' "$REPORT_TIMEOUT"
} > "$REPORT_DIR/environment.txt"

run_logged 'Building the GCC coverage executable...' "$REPORT_DIR/build.log" \
    make -C "$ROOT" PLATFORM="$PLATFORM" coverage -j2
run_logged 'Compiling instrumented unit tests and helpers...' "$REPORT_DIR/test-build.log" \
    make -C "$ROOT" PLATFORM="$PLATFORM" CC=gcc \
        CUP_TEST_CONFIGURATION=coverage test-unit-build test-helpers
export CUP_TEST_BINARY="$ROOT/build/$PLATFORM/coverage/bin/cup"
run_logged 'Running instrumented C unit tests...' "$REPORT_DIR/unit.log" \
    timeout --foreground --signal=TERM --kill-after=30s "$UNIT_TIMEOUT" \
        env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
        "$ROOT/tests/runners/unit.sh"
run_logged 'Running instrumented POSIX integration tests...' "$REPORT_DIR/integration.log" \
    env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
        CUP_TEST_SKIP_BUILD=1 CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
        "$ROOT/tests/runners/integration-posix.sh"

printf '==> Waiting for coverage counters to become stable...\n'
previous=
stable=0
attempt=1
while [ "$attempt" -le 15 ]; do
    current=$(find "$ROOT/build" -type f -name '*.gcda' -printf '%p:%s:%T@\n' | sort | cksum)
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

# Generate machine-readable reports first, then HTML when the core report succeeded.
common_args=(
    --root "$ROOT"
    --filter 'src/'
    --filter 'include/'
    --exclude 'tests/'
    --exclude 'build/'
    --merge-mode-functions separate
    --print-summary
)

run_gcovr() {
    jobs=$1
    timeout --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
        gcovr -j "$jobs" "${common_args[@]}" \
        --fail-under-line "$LINE_THRESHOLD" \
        --fail-under-branch "$BRANCH_THRESHOLD" \
        --fail-under-function "$FUNCTION_THRESHOLD" \
        --txt "$REPORT_DIR/summary.txt" \
        --xml "$REPORT_DIR/coverage.xml" --xml-pretty \
        --json "$REPORT_DIR/coverage.json" --json-pretty \
        --json-summary "$REPORT_DIR/coverage-summary.json" --json-summary-pretty
}

printf '==> Generating coverage reports...\n'
coverage_status=0
(cd "$ROOT" && run_gcovr "$REPORT_JOBS") >"$REPORT_DIR/gcovr.log" 2>&1 || coverage_status=$?
if [ "$coverage_status" -eq 124 ] || [ "$coverage_status" -eq 137 ]; then
    printf 'gcovr timed out; retrying with one worker.\n' >>"$REPORT_DIR/gcovr.log"
    coverage_status=0
    (cd "$ROOT" && run_gcovr 1) >>"$REPORT_DIR/gcovr.log" 2>&1 || coverage_status=$?
fi
reports_complete() {
    [ -s "$REPORT_DIR/coverage.json" ] &&
    [ -s "$REPORT_DIR/coverage.xml" ] &&
    [ -s "$REPORT_DIR/coverage-summary.json" ] &&
    [ -s "$REPORT_DIR/summary.txt" ] &&
    grep -Eq '"files"[[:space:]]*:[[:space:]]*\[[[:space:]]*\{' "$REPORT_DIR/coverage.json"
}
if [ "$coverage_status" -ne 0 ] && reports_complete; then
    printf 'Validating reports saved before gcovr exited...\n' >>"$REPORT_DIR/gcovr.log"
    coverage_status=0
    (cd "$ROOT" && gcovr --root "$ROOT" --merge-mode-functions separate \
        --add-tracefile "$REPORT_DIR/coverage.json" --print-summary \
        --fail-under-line "$LINE_THRESHOLD" \
        --fail-under-branch "$BRANCH_THRESHOLD" \
        --fail-under-function "$FUNCTION_THRESHOLD") \
        >>"$REPORT_DIR/gcovr.log" 2>&1 || coverage_status=$?
fi

html_status=0
if reports_complete; then
    printf '==> Rendering HTML coverage report...\n'
    (cd "$ROOT" && timeout --foreground --signal=TERM --kill-after=10s "$HTML_TIMEOUT" \
        gcovr --root "$ROOT" --merge-mode-functions separate \
        --add-tracefile "$REPORT_DIR/coverage.json" \
        --html-details "$REPORT_DIR/index.html" --no-html-syntax-highlighting) \
        >"$REPORT_DIR/html.log" 2>&1 || html_status=$?
else
    printf 'Coverage reports are incomplete.\n' >"$REPORT_DIR/html.log"
    [ "$coverage_status" -ne 0 ] || coverage_status=1
fi

cat "$REPORT_DIR/gcovr.log"
[ ! -f "$REPORT_DIR/summary.txt" ] || cat "$REPORT_DIR/summary.txt"
{
    printf 'coverage_line_threshold=%s%%\n' "$LINE_THRESHOLD"
    printf 'coverage_branch_threshold=%s%%\n' "$BRANCH_THRESHOLD"
    printf 'coverage_function_threshold=%s%%\n' "$FUNCTION_THRESHOLD"
} > "$REPORT_DIR/thresholds.env"
if [ "$html_status" -ne 0 ] && [ "$coverage_status" -eq 0 ]; then
    cat "$REPORT_DIR/html.log" >&2
    coverage_status=$html_status
fi
printf 'Coverage report written to %s\n' "$REPORT_DIR"
exit "$coverage_status"
