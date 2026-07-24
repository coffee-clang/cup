#!/usr/bin/env bash

# Purpose: Collects native C coverage on every supported platform and applies
# line, branch and function gates through the platform-native coverage backend.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
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
REPORT_DIR="${CUP_COVERAGE_DIR:-$ROOT/build/coverage/$PLATFORM}"
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
    cup_test_require_tool gcovr coverage || exit 2
    cup_test_require_tool gcov coverage || exit 2
fi
TIMEOUT_COMMAND=$(cup_test_find_timeout) || exit 2
if [ "$PLATFORM" = windows-x64 ]; then
    cup_test_require_tool powershell.exe 'Windows coverage integration tests' || exit 2
    cup_test_require_tool cygpath 'Windows coverage integration tests' || exit 2
fi
cup_test_require_dependencies

run_logged() {
    local label=$1
    local log_file=$2
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
    printf 'backend=%s\n' "$COVERAGE_BACKEND"
    printf 'compiler=%s\n' "$($CC --version | sed -n '1p')"
    if [ "$COVERAGE_BACKEND" = gcov ]; then
        printf 'gcovr=%s\n' "$(gcovr --version | sed -n '1p')"
        printf 'gcov=%s\n' "$(gcov --version | sed -n '1p')"
    else
        printf 'sdkroot=%s\n' "$SDKROOT"
        printf 'llvm_profdata=%s\n' "$($LLVM_PROFDATA --version | sed -n '1p')"
        printf 'llvm_cov=%s\n' "$($LLVM_COV --version | sed -n '1p')"
    fi
    printf 'unit_timeout_seconds=%s\n' "$UNIT_TIMEOUT"
    printf 'suite_timeout_seconds=%s\n' "$SUITE_TIMEOUT"
    printf 'report_timeout_seconds=%s\n' "$REPORT_TIMEOUT"
} >"$REPORT_DIR/environment.txt"

run_logged "Building the $COVERAGE_BACKEND coverage executable..." "$REPORT_DIR/build.log" \
    make -C "$ROOT" PLATFORM="$PLATFORM" CC="$CC" coverage -j2
run_logged 'Compiling instrumented unit tests and helpers...' "$REPORT_DIR/test-build.log" \
    make -C "$ROOT" PLATFORM="$PLATFORM" CC="$CC" \
        CUP_TEST_CONFIGURATION=coverage test-unit-build test-helpers

export CUP_TEST_BINARY="$ROOT/build/$PLATFORM/coverage/bin/cup"
[ "$PLATFORM" != windows-x64 ] || CUP_TEST_BINARY="$CUP_TEST_BINARY.exe"
if [ "$COVERAGE_BACKEND" = llvm ]; then
    mkdir -p "$REPORT_DIR/profiles"
    export LLVM_PROFILE_FILE="$REPORT_DIR/profiles/%m-%p.profraw"
fi

run_logged 'Running instrumented C unit tests...' "$REPORT_DIR/unit.log" \
    "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$UNIT_TIMEOUT" \
        env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
        "$ROOT/tests/runners/unit.sh"

if [ "$PLATFORM" = windows-x64 ]; then
    windows_runner=$(cygpath -w "$ROOT/tests/integration/windows/run.ps1")
    windows_binary=$(cygpath -w "$CUP_TEST_BINARY")
    run_logged 'Running instrumented Windows integration tests...' "$REPORT_DIR/integration.log" \
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=30s "$SUITE_TIMEOUT" \
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_runner" \
        -CupPath "$windows_binary"
else
    run_logged 'Running instrumented POSIX integration tests...' "$REPORT_DIR/integration.log" \
        env CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
            CUP_TEST_SUITE_TIMEOUT="$SUITE_TIMEOUT" \
            CUP_TEST_TIMEOUT_COMMAND="$TIMEOUT_COMMAND" \
            "$ROOT/tests/runners/integration-posix.sh"
fi

printf '==> Waiting for coverage counters to become stable...\n'
case "$COVERAGE_BACKEND" in
    gcov)
        counter_pattern='*.gcda'
        ;;
    llvm)
        counter_pattern='*.profraw'
        ;;
esac
previous=
stable=0
attempt=1
while [ "$attempt" -le 15 ]; do
    current=$(find "$ROOT/build" -type f -name "$counter_pattern" -exec cksum {} \; | sort | cksum)
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

if [ "$COVERAGE_BACKEND" = gcov ]; then
    coverage_log=$REPORT_DIR/gcovr.log
    common_args=(
        --root "$ROOT"
        --merge-mode-functions separate
        --print-summary
        --filter 'src/'
        --filter 'include/'
        --exclude 'tests/'
        --exclude 'build/'
    )

    run_gcovr() {
        jobs=$1
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
            gcovr -j "$jobs" "${common_args[@]}" \
            --txt "$REPORT_DIR/summary.txt" \
            --xml "$REPORT_DIR/coverage.xml" --xml-pretty \
            --json "$REPORT_DIR/coverage.json" --json-pretty \
            --json-summary "$REPORT_DIR/coverage-summary.json" --json-summary-pretty
    }

    printf '==> Generating gcovr coverage reports...\n'
    (cd "$ROOT" && run_gcovr "$REPORT_JOBS") >"$coverage_log" 2>&1 || generation_status=$?
    if [ "$generation_status" -eq 124 ] || [ "$generation_status" -eq 137 ]; then
        printf 'gcovr timed out; retrying with one worker.\n' >>"$coverage_log"
        generation_status=0
        (cd "$ROOT" && run_gcovr 1) >>"$coverage_log" 2>&1 || generation_status=$?
    fi

    reports_complete() {
        [ -s "$REPORT_DIR/coverage.json" ] &&
        [ -s "$REPORT_DIR/coverage.xml" ] &&
        [ -s "$REPORT_DIR/coverage-summary.json" ] &&
        [ -s "$REPORT_DIR/summary.txt" ] &&
        grep -Eq '"files"[[:space:]]*:' "$REPORT_DIR/coverage.json"
    }

    if reports_complete && [ "$generation_status" -eq 0 ]; then
        printf '==> Validating coverage thresholds from the saved tracefile...\n' >>"$coverage_log"
        (cd "$ROOT" && "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
            gcovr --root "$ROOT" --merge-mode-functions separate \
            --add-tracefile "$REPORT_DIR/coverage.json" --print-summary \
            --fail-under-line "$LINE_THRESHOLD" \
            --fail-under-branch "$BRANCH_THRESHOLD" \
            --fail-under-function "$FUNCTION_THRESHOLD") \
            >>"$coverage_log" 2>&1 || threshold_status=$?
    fi

    if reports_complete; then
        printf '==> Rendering HTML coverage report...\n'
        (cd "$ROOT" && "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$HTML_TIMEOUT" \
            gcovr --root "$ROOT" --merge-mode-functions separate \
            --add-tracefile "$REPORT_DIR/coverage.json" \
            --html-details "$REPORT_DIR/index.html" --no-html-syntax-highlighting) \
            >"$REPORT_DIR/html.log" 2>&1 || html_status=$?
    else
        printf 'Coverage reports are incomplete.\n' >"$REPORT_DIR/html.log"
        [ "$generation_status" -ne 0 ] || generation_status=1
    fi
else
    coverage_log=$REPORT_DIR/llvm-cov.log
    : >"$coverage_log"
    profile_files=()
    llvm_objects=()
    llvm_sources=()
    while IFS= read -r profile; do
        [ -n "$profile" ] && profile_files+=("$profile")
    done < <(find "$REPORT_DIR/profiles" -type f -name '*.profraw' | sort)
    while IFS= read -r binary; do
        [ -n "$binary" ] && llvm_objects+=(--object "$binary")
    done < <(find "$ROOT/build/$PLATFORM/coverage/tests" -type f -perm -111 | sort)
    while IFS= read -r source; do
        [ -n "$source" ] && llvm_sources+=(--sources "$source")
    done < <(find "$ROOT/src" "$ROOT/include" -type f \
        \( -name '*.c' -o -name '*.h' \) | sort)

    if [ "${#profile_files[@]}" -eq 0 ] || [ "${#llvm_sources[@]}" -eq 0 ]; then
        printf 'LLVM coverage inputs are incomplete.\n' >>"$coverage_log"
        generation_status=1
    fi

    llvm_common=(
        "$CUP_TEST_BINARY"
        "${llvm_objects[@]}"
        -instr-profile="$REPORT_DIR/coverage.profdata"
        --path-equivalence="/usr/src/cup,$ROOT"
        --show-branch-summary
        "${llvm_sources[@]}"
    )

    run_llvm_step() {
        label=$1
        shift
        printf '==> %s\n' "$label" >>"$coverage_log"
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
            "$@" >>"$coverage_log" 2>&1
    }
    run_llvm_capture() {
        label=$1
        output=$2
        shift 2
        printf '==> %s\n' "$label" >>"$coverage_log"
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$REPORT_TIMEOUT" \
            "$@" >"$output" 2>>"$coverage_log"
    }

    if [ "$generation_status" -eq 0 ]; then
        run_llvm_step 'Merging LLVM profiles...' \
            "$LLVM_PROFDATA" merge -sparse "${profile_files[@]}" \
            -o "$REPORT_DIR/coverage.profdata" || generation_status=$?
    fi
    if [ "$generation_status" -eq 0 ]; then
        run_llvm_capture 'Generating LLVM text report...' "$REPORT_DIR/summary.txt" \
            "$LLVM_COV" report "${llvm_common[@]}" || generation_status=$?
    fi
    if [ "$generation_status" -eq 0 ]; then
        run_llvm_capture 'Exporting LLVM coverage JSON...' "$REPORT_DIR/coverage.json" \
            "$LLVM_COV" export "${llvm_common[@]}" || generation_status=$?
    fi
    if [ "$generation_status" -eq 0 ]; then
        run_llvm_capture 'Exporting LLVM coverage summary...' \
            "$REPORT_DIR/coverage-summary.json" \
            "$LLVM_COV" export --summary-only "${llvm_common[@]}" || generation_status=$?
    fi
    if [ "$generation_status" -eq 0 ]; then
        run_llvm_capture 'Exporting LCOV tracefile...' "$REPORT_DIR/coverage.lcov" \
            "$LLVM_COV" export -format=lcov "${llvm_common[@]}" || generation_status=$?
    fi

    reports_complete() {
        [ -s "$REPORT_DIR/coverage.profdata" ] &&
        [ -s "$REPORT_DIR/coverage.json" ] &&
        [ -s "$REPORT_DIR/coverage-summary.json" ] &&
        [ -s "$REPORT_DIR/coverage.lcov" ] &&
        [ -s "$REPORT_DIR/summary.txt" ] &&
        grep -Eq '"files"[[:space:]]*:' "$REPORT_DIR/coverage.json"
    }

    if reports_complete && [ "$generation_status" -eq 0 ]; then
        metrics=$(awk '
            $1 == "TOTAL" {
                gsub(/%/, "", $7)
                gsub(/%/, "", $10)
                gsub(/%/, "", $13)
                print $10, $13, $7, $8, $11, $5
                found = 1
            }
            END { if (!found) exit 1 }
        ' "$REPORT_DIR/summary.txt") || generation_status=1
        if [ "$generation_status" -eq 0 ]; then
            read -r line_coverage branch_coverage function_coverage \
                line_total branch_total function_total <<EOF
$metrics
EOF
            if [ "$line_total" -eq 0 ] || [ "$branch_total" -eq 0 ] || \
                    [ "$function_total" -eq 0 ]; then
                printf 'LLVM coverage report contains empty metrics.\n' >>"$coverage_log"
                generation_status=1
            else
                {
                    printf 'coverage_lines=%s%%\n' "$line_coverage"
                    printf 'coverage_branches=%s%%\n' "$branch_coverage"
                    printf 'coverage_functions=%s%%\n' "$function_coverage"
                } >"$REPORT_DIR/coverage-metrics.env"
                for metric in \
                        "lines:$line_coverage:$LINE_THRESHOLD" \
                        "branches:$branch_coverage:$BRANCH_THRESHOLD" \
                        "functions:$function_coverage:$FUNCTION_THRESHOLD"; do
                    name=${metric%%:*}
                    remainder=${metric#*:}
                    actual=${remainder%%:*}
                    minimum=${remainder#*:}
                    if awk -v actual="$actual" -v minimum="$minimum" \
                            'BEGIN { exit !(actual + 0 < minimum + 0) }'; then
                        printf 'Coverage for %s is %s%%, below the required %s%%.\n' \
                            "$name" "$actual" "$minimum" >>"$coverage_log"
                        threshold_status=1
                    fi
                done
            fi
        fi
    elif [ "$generation_status" -eq 0 ]; then
        printf 'Coverage reports are incomplete.\n' >>"$coverage_log"
        generation_status=1
    fi

    if reports_complete; then
        printf '==> Rendering LLVM HTML coverage report...\n'
        mkdir -p "$REPORT_DIR/html"
        "$TIMEOUT_COMMAND" --foreground --signal=TERM --kill-after=10s "$HTML_TIMEOUT" \
            "$LLVM_COV" show "${llvm_common[@]}" -format=html \
            -output-dir="$REPORT_DIR/html" >"$REPORT_DIR/html.log" 2>&1 || html_status=$?
    else
        printf 'Coverage reports are incomplete.\n' >"$REPORT_DIR/html.log"
        [ "$generation_status" -ne 0 ] || generation_status=1
    fi
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
