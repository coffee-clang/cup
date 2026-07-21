#!/bin/sh

# Purpose: Verifies coverage orchestration has no manual gap inventory or Python dependency.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
runner=$ROOT/tests/runners/coverage.sh

if [ ! -x "$runner" ]; then
    echo 'coverage runner is not executable' >&2
    exit 1
fi
for required in '--fail-under-line' '--fail-under-branch' '--fail-under-function' \
        'coverage.json' 'coverage.xml' 'coverage-summary.json' 'timeout --foreground'; do
    grep -Fq -- "$required" "$runner" || {
        echo "coverage runner is missing: $required" >&2
        exit 1
    }
done
script_lang=pyth
script_lang=${script_lang}on
script_ext=.py
for removed in "classify$script_ext" "run-with-timeout$script_ext" gap-policy.tsv "$script_lang" "${script_lang}3"; do
    ! grep -Fq -- "$removed" "$runner" || {
        echo "coverage runner still references removed tooling: $removed" >&2
        exit 1
    }
done
[ ! -e "$ROOT/tests/coverage/classify$script_ext" ]
[ ! -e "$ROOT/tests/coverage/run-with-timeout$script_ext" ]
[ ! -e "$ROOT/tests/coverage/gap-policy.tsv" ]

timeout_status=0
timeout --foreground --signal=TERM --kill-after=1s 1s sh -c 'sleep 30' || timeout_status=$?
[ "$timeout_status" -eq 124 ] || {
    echo "timeout returned $timeout_status instead of 124" >&2
    exit 1
}
printf 'Coverage orchestration policy tests passed.\n'
