#!/usr/bin/env bash

# Purpose: Captures macOS LLVM coverage evidence without changing the official
# coverage gate. It compares prefix-map variants, a real CUP executable and a
# real unit-test binary, then preserves raw profiles and exported JSON.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
PLATFORM="$CUP_TEST_PLATFORM"

case "$PLATFORM" in
    macos-x64|macos-arm64) ;;
    *)
        printf 'macOS coverage diagnostics do not support %s.\n' "$PLATFORM" >&2
        exit 2
        ;;
esac

OUT="${CUP_MACOS_COVERAGE_DIAGNOSTICS_DIR:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/cup-macos-coverage-diagnostics/$PLATFORM}"
case "$OUT" in
    ''|/|"$HOME"|"$ROOT"|"$ROOT"/*)
        printf 'Unsafe macOS coverage diagnostic output path: %s\n' "$OUT" >&2
        exit 2
        ;;
    */cup-macos-coverage-diagnostics/$PLATFORM) ;;
    *)
        printf 'Diagnostic output path must end in '
        printf 'cup-macos-coverage-diagnostics/%s: %s\n' "$PLATFORM" "$OUT" >&2
        exit 2
        ;;
esac
rm -rf -- "$OUT"
mkdir -p "$OUT"
: >"$OUT/status.env"

failures=0
record_failure() {
    failures=$((failures + 1))
    printf 'diagnostic_failure=%s\n' "$1" >>"$OUT/status.env"
}

write_summary() {
    script_status=$1
    set +e
    mkdir -p "$OUT"
    {
        printf '# macOS coverage diagnostic summary\n\n'
        printf -- '- Platform: `%s`\n' "$PLATFORM"
        if [ -n "${CLANG:-}" ] && [ -x "${CLANG:-}" ]; then
            printf -- '- Toolchain: `%s`\n' \
                "$($CLANG --version 2>/dev/null | sed -n '1p')"
        else
            printf -- '- Toolchain: `not resolved`\n'
        fi
        if command -v gcovr >/dev/null 2>&1; then
            printf -- '- gcovr: `%s`\n' \
                "$(gcovr --version 2>/dev/null | sed -n '1p')"
        else
            printf -- '- gcovr: `not resolved`\n'
        fi
        printf -- '- Recorded diagnostic failures: `%s`\n' "${failures:-0}"
        printf -- '- Script exit status: `%s`\n\n' "$script_status"
        printf 'Inspect `toolchain.txt`, each `llvm-cov-summary.txt`, and '
        printf '`full-unit-gcovr/gcovr.log` in the uploaded artifact.\n'
    } >"$OUT/summary.md"
    printf 'script_exit_status=%s\n' "$script_status" >>"$OUT/status.env"
}

trap 'write_summary "$?"' EXIT

cup_test_require_dependencies
cup_test_require_tool xcrun 'macOS coverage diagnostics'
cup_test_require_tool gcovr 'macOS coverage diagnostics'
cup_test_require_tool python3 'macOS coverage diagnostics'

SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
export SDKROOT
CLANG=$(xcrun --sdk macosx --find clang)
LLVM_PROFDATA=$(xcrun --sdk macosx --find llvm-profdata)
LLVM_COV=$(xcrun --sdk macosx --find llvm-cov)

run_capture() {
    label=$1
    log=$2
    shift 2
    printf '==> %s\n' "$label"
    if "$@" >"$log" 2>&1; then
        status=0
    else
        status=$?
    fi
    printf '%s_status=%s\n' "$label" "$status" | tr ' -' '__' >>"$OUT/status.env"
    if [ "$status" -ne 0 ]; then
        cat "$log" >&2
    fi
    return "$status"
}

{
    printf 'platform=%s\n' "$PLATFORM"
    printf 'runner_os=%s\n' "$(sw_vers -productVersion)"
    printf 'developer_dir=%s\n' "$(xcode-select -p)"
    printf 'sdk_root=%s\n' "$SDKROOT"
    printf 'xcrun_clang=%s\n' "$CLANG"
    printf 'xcrun_llvm_profdata=%s\n' "$LLVM_PROFDATA"
    printf 'xcrun_llvm_cov=%s\n' "$LLVM_COV"
    printf 'path_clang=%s\n' "$(command -v clang || :)"
    printf 'path_llvm_profdata=%s\n' "$(command -v llvm-profdata || :)"
    printf 'path_llvm_cov=%s\n' "$(command -v llvm-cov || :)"
    printf 'clang_version=%s\n' "$($CLANG --version | sed -n '1p')"
    printf 'llvm_profdata_version=%s\n' "$($LLVM_PROFDATA --version | sed -n '1p')"
    printf 'llvm_cov_version=%s\n' "$($LLVM_COV --version | sed -n '1p')"
    printf 'gcovr_version=%s\n' "$(gcovr --version | sed -n '1p')"
    printf 'python_version=%s\n' "$(python3 --version 2>&1)"
} >"$OUT/toolchain.txt"

PROBE_SOURCE="$OUT/branch_probe.c"
cat >"$PROBE_SOURCE" <<'PROBE'
#include <stdio.h>
#include <stdlib.h>

static int classify(int value) {
    if (value < 0) {
        return -1;
    }
    if (value == 0) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int value;
    if (argc != 2) {
        return 2;
    }
    value = atoi(argv[1]);
    printf("%d\n", classify(value));
    return 0;
}
PROBE

summarize_export() {
    input=$1
    output=$2
    python3 - "$input" >"$output" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
files = []
totals = {}
for block in data.get("data", []):
    files.extend(block.get("files", []))
    summary = block.get("totals") or block.get("summary")
    if isinstance(summary, dict):
        totals = summary
print(f"export_type={data.get('type', '')}")
print(f"export_version={data.get('version', '')}")
print(f"file_count={len(files)}")
for item in files[:30]:
    print(f"file={item.get('filename', '')}")
    summary = item.get("summary", {})
    for metric in ("lines", "functions", "branches", "regions", "mcdc"):
        values = summary.get(metric)
        if isinstance(values, dict):
            print(
                f"file_{metric}="
                f"{values.get('covered', 0)}/{values.get('count', 0)}"
            )
for metric in ("lines", "functions", "branches", "regions", "mcdc"):
    values = totals.get(metric)
    if isinstance(values, dict):
        print(f"total_{metric}={values.get('covered', 0)}/{values.get('count', 0)}")
PY
}

merge_profiles() {
    profile_dir=$1
    output=$2
    set -- "$profile_dir"/*.profraw
    [ -f "$1" ] || return 1
    "$LLVM_PROFDATA" merge -sparse "$@" -o "$output"
}

run_probe_variant() {
    variant=$1
    shift
    directory="$OUT/probe-$variant"
    binary="$directory/branch-probe"
    mkdir -p "$directory/profiles"

    if ! run_capture "probe_${variant}_compile" "$directory/compile.log" \
        "$CLANG" -isysroot "$SDKROOT" -O0 -g \
        -fprofile-instr-generate -fcoverage-mapping \
        "$@" "$PROBE_SOURCE" -o "$binary"; then
        record_failure "probe_${variant}_compile"
        return
    fi

    for value in -1 0 1; do
        LLVM_PROFILE_FILE="$directory/profiles/%m-%p.profraw" \
            "$binary" "$value" >>"$directory/run.log" 2>&1 || \
            record_failure "probe_${variant}_run_${value}"
    done

    if ! merge_profiles "$directory/profiles" "$directory/merged.profdata"; then
        record_failure "probe_${variant}_merge"
        return
    fi
    run_capture "probe_${variant}_report" "$directory/llvm-cov-report.txt" \
        "$LLVM_COV" report "$binary" \
        -instr-profile="$directory/merged.profdata" || \
        record_failure "probe_${variant}_report"
    if "$LLVM_COV" export "$binary" \
        -instr-profile="$directory/merged.profdata" \
        >"$directory/llvm-cov-export.json" 2>"$directory/llvm-cov-export.err"; then
        summarize_export "$directory/llvm-cov-export.json" \
            "$directory/llvm-cov-summary.txt"
    else
        record_failure "probe_${variant}_export"
    fi

    gcovr_status=0
    PATH="$(dirname "$LLVM_COV"):$PATH" gcovr \
        --root "$ROOT" \
        --llvm-profdata-executable "$LLVM_PROFDATA" \
        --llvm-cov-binary "$binary" \
        "$directory/profiles" \
        --print-summary \
        --txt "$directory/gcovr-summary.txt" \
        --json "$directory/gcovr.json" --json-pretty \
        >"$directory/gcovr.log" 2>&1 || gcovr_status=$?
    printf 'probe_%s_gcovr_status=%s\n' "$variant" "$gcovr_status" \
        >>"$OUT/status.env"
}

run_probe_variant plain
run_probe_variant reproducible \
    "-ffile-prefix-map=$ROOT=/usr/src/cup" \
    "-fdebug-prefix-map=$ROOT=/usr/src/cup" \
    "-fmacro-prefix-map=$ROOT=/usr/src/cup"
run_probe_variant coverage-prefix \
    "-ffile-prefix-map=$ROOT=/usr/src/cup" \
    "-fdebug-prefix-map=$ROOT=/usr/src/cup" \
    "-fmacro-prefix-map=$ROOT=/usr/src/cup" \
    "-fcoverage-prefix-map=$ROOT=."

printf '==> Building real CUP coverage binaries with the xcrun toolchain...\n'
if ! make -C "$ROOT" clean >"$OUT/cup-clean.log" 2>&1; then
    record_failure cup_clean
fi
if ! make -C "$ROOT" PLATFORM="$PLATFORM" DEPS_PREFIX="$DEPS_PREFIX" \
    CC="$CLANG" EXTRA_CFLAGS="-isysroot $SDKROOT" \
    EXTRA_LDFLAGS="-isysroot $SDKROOT" coverage -j2 >"$OUT/cup-build.log" 2>&1; then
    cat "$OUT/cup-build.log" >&2
    record_failure cup_build
fi
if ! make -C "$ROOT" PLATFORM="$PLATFORM" DEPS_PREFIX="$DEPS_PREFIX" \
    CC="$CLANG" EXTRA_CFLAGS="-isysroot $SDKROOT" \
    EXTRA_LDFLAGS="-isysroot $SDKROOT" \
    CUP_TEST_CONFIGURATION=coverage test-unit-build \
    >"$OUT/unit-build.log" 2>&1; then
    cat "$OUT/unit-build.log" >&2
    record_failure unit_build
fi

CUP_BINARY="$ROOT/build/$PLATFORM/coverage/bin/cup"
UNIT_BINARY="$ROOT/build/$PLATFORM/coverage/tests/unit/test_command_context"

profile_real_binary() {
    name=$1
    binary=$2
    shift 2
    directory="$OUT/real-$name"
    mkdir -p "$directory/profiles"
    if [ ! -x "$binary" ]; then
        record_failure "real_${name}_missing"
        return
    fi
    LLVM_PROFILE_FILE="$directory/profiles/%m-%p.profraw" \
        "$binary" "$@" >"$directory/run.log" 2>&1 || \
        record_failure "real_${name}_run"
    if ! merge_profiles "$directory/profiles" "$directory/merged.profdata"; then
        record_failure "real_${name}_merge"
        return
    fi
    "$LLVM_COV" report "$binary" -instr-profile="$directory/merged.profdata" \
        >"$directory/llvm-cov-report.txt" 2>"$directory/llvm-cov-report.err" || \
        record_failure "real_${name}_report"
    if "$LLVM_COV" export "$binary" -instr-profile="$directory/merged.profdata" \
        >"$directory/llvm-cov-export.json" 2>"$directory/llvm-cov-export.err"; then
        summarize_export "$directory/llvm-cov-export.json" \
            "$directory/llvm-cov-summary.txt"
    else
        record_failure "real_${name}_export"
    fi
}

profile_real_binary cup "$CUP_BINARY" --version
profile_real_binary unit "$UNIT_BINARY"

FULL_DIR="$OUT/full-unit-gcovr"
mkdir -p "$FULL_DIR/profiles"
if [ -d "$ROOT/build/$PLATFORM/coverage/tests/unit" ]; then
    LLVM_PROFILE_FILE="$FULL_DIR/profiles/%m-%p.profraw" \
        CUP_TEST_CONFIGURATION=coverage CUP_TEST_PLATFORM="$PLATFORM" \
        "$ROOT/tests/runners/unit.sh" >"$FULL_DIR/unit.log" 2>&1 || \
        record_failure full_unit_run
fi

backend_args=(
    --llvm-profdata-executable "$LLVM_PROFDATA"
    --llvm-cov-binary "$CUP_BINARY"
)
if [ -d "$ROOT/build/$PLATFORM/coverage/tests/unit" ]; then
    while IFS= read -r binary; do
        [ -n "$binary" ] && backend_args+=(--llvm-cov-binary "$binary")
    done < <(find "$ROOT/build/$PLATFORM/coverage/tests/unit" \
        -type f -perm -111 | sort)
fi
full_gcovr_status=0
PATH="$(dirname "$LLVM_COV"):$PATH" gcovr \
    --root "$ROOT" \
    --merge-mode-functions separate \
    "${backend_args[@]}" \
    "$FULL_DIR/profiles" \
    --print-summary \
    --txt "$FULL_DIR/summary.txt" \
    --json "$FULL_DIR/coverage.json" --json-pretty \
    --json-summary "$FULL_DIR/coverage-summary.json" --json-summary-pretty \
    >"$FULL_DIR/gcovr.log" 2>&1 || full_gcovr_status=$?
printf 'full_unit_gcovr_status=%s\n' "$full_gcovr_status" >>"$OUT/status.env"

# The workflow is evidentiary rather than a gate. Build/tool failures are
# recorded in status.env and preserved as artifacts instead of hiding later
# diagnostics behind the first non-zero command.
printf 'macOS coverage diagnostics written to %s\n' "$OUT"
exit 0
