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

tests_job_condition() {
    file=$1
    job=$2
    awk -v job="$job" '
        $0 == "  " job ":" { in_job = 1; next }
        in_job && $0 ~ /^  [^ ]/ { exit }
        in_job && $0 ~ /^    if:[[:space:]]*/ {
            sub(/^    if:[[:space:]]*/, "")
            print
            found = 1
            exit
        }
        END { if (!found) exit 1 }
    ' "$file"
}

tests_cancellation_policy() {
    file=$1
    for job in posix windows coverage sanitizers gate; do
        condition=$(tests_job_condition "$file" "$job") || return 1
        [ "$condition" = '${{ !cancelled() }}' ] || return 1
    done
}

tests_cancellation_policy "$workflows/tests.yml" ||
    fail 'Tests diagnostic jobs do not preserve failure diagnostics while respecting cancellation'

cancel_fixture=$TMP_ROOT/tests-cancellation.yml
cat >"$cancel_fixture" <<'EOF_CANCEL_FIXTURE'
jobs:
  posix:
    if: ${{ !cancelled() }}
  windows:
    if: ${{ !cancelled() }}
  coverage:
    if: ${{ !cancelled() }}
  sanitizers:
    if: ${{ !cancelled() }}
  gate:
    if: ${{ !cancelled() }}
EOF_CANCEL_FIXTURE
tests_cancellation_policy "$cancel_fixture" ||
    fail 'Tests cancellation-policy checker rejected its valid control fixture'
awk '!changed && /!cancelled\(\)/ { sub(/!cancelled\(\)/, "always()"); changed = 1 }
    { print }' "$cancel_fixture" >"$cancel_fixture.bad"
if tests_cancellation_policy "$cancel_fixture.bad"; then
    fail 'Tests cancellation-policy checker accepted an always-run diagnostic job'
fi

grep -Fq 'name: cup-source-build-config-${{ matrix.platform }}-attempt-${{ github.run_attempt }}' \
    "$workflows/tests.yml" ||
    fail 'Tests workflow does not publish run-attempt-bound source build identity'
grep -Fq 'CUP_SOURCE_BUILD_CONFIG: ${{ runner.temp }}/build-config.txt' \
    "$workflows/tests.yml" ||
    fail 'POSIX source tests do not preserve the canonical build-config artifact basename'
grep -Fq 'path: ${{ runner.temp }}/build-config.txt' "$workflows/tests.yml" ||
    fail 'Tests workflow does not upload the canonical source build-config basename'
grep -Fq 'name: cup-source-build-config-${{ matrix.platform }}-attempt-${{ needs.metadata.outputs.tests_run_attempt }}' \
    "$workflows/release.yml" ||
    fail 'release workflow does not select source build identity from the tested run attempt'
grep -Fq 'run-id: ${{ needs.metadata.outputs.tests_run_id }}' "$workflows/release.yml" ||
    fail 'release workflow does not bind source build identity to the selected Tests run'
grep -Fq 'tests_run_attempt:' "$workflows/release.yml" ||
    fail 'release workflow does not expose the Tests run attempt'
# Release-run artifacts use stable names so a failed publisher can be retried without
# requiring already-successful upstream jobs to run again. Rebuilt jobs replace them explicitly.
for artifact in cup-release-common 'cup-release-${{ matrix.platform }}' 'cup-symbols-${{ matrix.platform }}'; do
    grep -Fq "name: $artifact" "$workflows/release.yml" ||
        fail "release workflow is missing stable internal artifact name: $artifact"
done
if grep -Eq 'cup-(release|symbols)-[^ ]*attempt-\$\{\{ github\.run_attempt \}\}' "$workflows/release.yml"; then
    fail 'release workflow still binds internal artifacts to the retry attempt'
fi
[ "$(grep -c 'overwrite: true' "$workflows/release.yml")" -eq 3 ] ||
    fail 'release workflow does not explicitly replace rerun-produced internal artifacts'
grep -Fq 'candidate=$(cygpath -u "$RUNNER_TEMP")/cup-release-windows-x64' \
    "$workflows/release.yml" ||
    fail 'release workflow does not normalize the Windows candidate path for POSIX assembly'

printf 'CI security policy tests passed.\n'
