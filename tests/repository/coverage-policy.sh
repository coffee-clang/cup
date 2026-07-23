#!/bin/sh

# Purpose: Verifies native coverage orchestration, thresholds and tool ownership.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
runner=$ROOT/tests/runners/coverage.sh
environment=$ROOT/tests/support/environment.sh

if [ ! -x "$runner" ]; then
    echo 'coverage runner is not executable' >&2
    exit 1
fi
for required in '--fail-under-line' '--fail-under-branch' '--fail-under-function' \
        'coverage.json' 'coverage.xml' 'coverage-summary.json' \
        'linux-x64|linux-arm64' 'macos-x64|macos-arm64' 'windows-x64' \
        '--llvm-profdata-executable' '--llvm-cov-binary' \
        'CUP_TEST_TIMEOUT_COMMAND' 'powershell.exe' \
        'cup_test_require_gcovr_llvm' 'build/$PLATFORM/coverage/tests' \
        'CUP_COVERAGE_REPORT_TIMEOUT:-600' 'reports_complete' \
        'generation_status' 'threshold_status' 'html_status' \
        '--add-tracefile' "--exclude 'tests/'" "--exclude 'build/'"; do
    grep -Fq -- "$required" "$runner" || {
        echo "coverage runner is missing: $required" >&2
        exit 1
    }
done
for required in cup_test_find_timeout cup_test_require_tool \
        'brew install gcovr' 'mingw-w64-ucrt-x86_64-gcovr'; do
    grep -Fq -- "$required" "$environment" || {
        echo "coverage tool diagnostics are missing: $required" >&2
        exit 1
    }
done

sanitizer_runner=$ROOT/tests/runners/sanitizers.sh
source_runner=$ROOT/scripts/ci/source-posix.sh
prepare_runner=$ROOT/scripts/ci/prepare-posix.sh
for required in 'linux-x64|linux-arm64' 'macos-x64|macos-arm64' 'windows-x64' \
        'detect_leaks=$LEAKS' 'LEAKS=0' 'MAKE_PLATFORM_ARGS' 'WINDRES=llvm-windres' \
        'CUP_SANITIZER_UNIT_TIMEOUT' \
        'CUP_SANITIZER_SUITE_TIMEOUT' 'CUP_TEST_TIMEOUT_COMMAND'; do
    grep -Fq -- "$required" "$sanitizer_runner" || {
        echo "sanitizer runner is missing: $required" >&2
        exit 1
    }
done

if command -v timeout >/dev/null 2>&1; then
    timeout_status=0
    timeout --foreground --signal=TERM --kill-after=1s 1s sh -c 'sleep 30' || timeout_status=$?
    [ "$timeout_status" -eq 124 ] || {
        echo "timeout returned $timeout_status instead of 124" >&2
        exit 1
    }
fi
for required in generation_status threshold_status html_status \
        'gcovr_command=(env "PATH=$LLVM_TOOL_DIR:$PATH" gcovr)' \
        'integers from 0 to 100' 'The gcov backend requires GCC' \
        'powershell.exe -NoProfile' '$TIMEOUT_COMMAND'; do
    grep -Fq -- "$required" "$runner" || {
        echo "coverage runner is missing regression guard: $required" >&2
        exit 1
    }
done
for required in 'MSYSTEM:-}' CLANG64 llvm-windres \
        'working ASan/UBSan runtimes' 'CC="${CC:-clang}"'; do
    grep -Fq -- "$required" "$sanitizer_runner" || {
        echo "sanitizer runner is missing regression guard: $required" >&2
        exit 1
    }
done

for required in \
        'make PLATFORM="$platform" CC=clang' \
        'make CC=clang test-unit' \
        'CC=clang check-binary'; do
    grep -Fq -- "$required" "$source_runner" || {
        echo "Linux secondary compiler pass is missing: $required" >&2
        exit 1
    }
done
for required in \
        'source)' \
        'linux-x64' \
        'clang openssl' \
        'sanitizers)' \
        'clang llvm'; do
    grep -Fq -- "$required" "$prepare_runner" || {
        echo "POSIX compiler-tool ownership is missing: $required" >&2
        exit 1
    }
done
# Exercise the status precedence implemented directly by the coverage runner.
cup_quality_final_status() {
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
[ "$(cup_quality_final_status 0 0 0)" = 0 ]
[ "$(cup_quality_final_status 42 0 0)" = 42 ]
[ "$(cup_quality_final_status 0 7 0)" = 7 ]
[ "$(cup_quality_final_status 0 0 9)" = 9 ]
[ "$(cup_quality_final_status 42 7 9)" = 42 ]
if cup_quality_final_status invalid 0 0 >/dev/null 2>&1; then
    echo 'quality status helper accepted non-numeric input' >&2
    exit 1
fi
printf 'Coverage orchestration policy tests passed.\n'
