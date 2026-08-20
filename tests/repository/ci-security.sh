#!/bin/sh

# Verifies readable numeric external action refs and least-privilege workflow permissions.
set -eu
TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"
test_begin ci-security

workflows=$PROJECT_ROOT/.github/workflows

bad_refs=
for workflow_file in "$workflows"/*.yml; do
    [ "${workflow_file##*/}" = static.yml ] && continue
    invalid=$(grep -nE '^[[:space:]]*-?[[:space:]]*uses:[[:space:]]+[^./][^[:space:]#]*@' \
        "$workflow_file" |
        grep -Ev '@v[0-9]+(\.[0-9]+){0,2}([[:space:]]+#.*)?[[:space:]]*$' || true)
    [ -z "$invalid" ] || bad_refs="${bad_refs}${workflow_file}:$invalid
"
done
[ -z "$bad_refs" ] || fail "modifiable workflow action does not use a readable numeric version ref:
$bad_refs"

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

grep -Fq 'cup-tests-evidence-index-attempt-${{ github.run_attempt }}' "$workflows/tests.yml" ||
    fail 'Tests workflow does not publish run-attempt-bound evidence index'
grep -Fq 'artifact-ids: ${{ matrix.dependency_artifact_id }}' "$workflows/release.yml" ||
    fail 'release workflow does not select dependency evidence by artifact ID'
grep -Fq 'tests_run_attempt:' "$workflows/release.yml" ||
    fail 'release workflow does not expose the Tests run attempt'

printf 'CI security policy tests passed.\n'
