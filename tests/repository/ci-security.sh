#!/bin/sh

# Verifies the workflow permission boundary that protects release authority.
set -eu
TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"
test_begin ci-security

workflows=$PROJECT_ROOT/.github/workflows

checkout_failures=
for workflow_file in "$workflows"/*.yml; do
    [ "${workflow_file##*/}" = static.yml ] && continue
    grep -nE 'uses:[[:space:]]*actions/checkout@v[0-9]+(\.[0-9]+){0,2}([[:space:]]|$)' "$workflow_file" |
        while IFS=: read -r line_number remainder; do
            end_line=$((line_number + 8))
            sed -n "${line_number},${end_line}p" "$workflow_file" |
                grep -Eq 'persist-credentials:[[:space:]]*false' || {
                    printf '%s:%s\n' "$workflow_file" "$line_number"
                    exit 1
                }
        done || checkout_failures=1
done
[ -z "$checkout_failures" ] || fail 'checkout does not disable persisted credentials'

for workflow in dependencies.yml debug.yml release.yml static.yml tests.yml; do
    grep -Eq '^permissions:$' "$workflows/$workflow" ||
        fail "$workflow has no explicit top-level permissions"
done
! grep -RInE 'permissions:[[:space:]]*(write-all|read-all)' "$workflows" >/dev/null ||
    fail 'workflow uses a broad permissions shortcut'
write_permissions=$(
    grep -RhcE '^[[:space:]]+contents:[[:space:]]+write$' "$workflows"/*.yml |
        awk '{count += $1} END {print count + 0}')
[ "$write_permissions" -eq 1 ] ||
    fail 'contents: write must appear only in the release publisher'

printf 'CI release-authority permission tests passed.\n'
