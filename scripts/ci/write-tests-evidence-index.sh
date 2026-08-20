#!/bin/sh

# Commits one canonical run-attempt-bound index of release evidence artifacts.
set -eu
LC_ALL=C
LANG=C
export LC_ALL LANG
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"
. "$SCRIPT_DIR/tests-evidence-artifacts.sh"

[ "$#" -eq 6 ] || {
    printf 'Usage: %s <output-dir> <artifacts-json> <repository> <commit> <run-id> <run-attempt>\n' \
        "$0" >&2
    exit 2
}

OUTPUT=$1
JSON=$2
REPOSITORY=$3
COMMIT=$4
RUN_ID=$5
RUN_ATTEMPT=$6
WORKING_DIRECTORY=$(pwd -P)
OUTPUT=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$OUTPUT")
JSON=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$JSON")

ci_evidence_validate_repository "$REPOSITORY"
ci_evidence_validate_sha "$COMMIT"
ci_evidence_validate_number "$RUN_ID"
ci_evidence_validate_number "$RUN_ATTEMPT"
cup_path_require_regular_file "$JSON" 'artifact API response' ||
    ci_evidence_fail 'unsafe artifact API response'
command -v jq >/dev/null 2>&1 || ci_evidence_fail 'jq is required'

expected=$(ci_tests_evidence_names "$RUN_ATTEMPT")
records=$(mktemp "${TMPDIR:-/tmp}/cup-tests-evidence-records.XXXXXX")
records_one=$records.one

cleanup_records() {
    rm -f -- "$records" "$records_one"
}

cleanup_index() {
    cleanup_records

    if [ -n "${CI_EVIDENCE_STAGING:-}" ]; then
        cup_path_remove_directory_tree \
            "$CI_EVIDENCE_STAGING" 'tests evidence index staging' \
            >/dev/null 2>&1 || true
    fi
}

trap cleanup_index EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf '%s\n' "$expected" | while IFS= read -r name; do
    [ -n "$name" ] || continue

    count=$(jq --arg name "$name" \
        '[.artifacts[] | select(.name == $name and .expired == false)] | length' \
        "$JSON")
    [ "$count" = 1 ] ||
        ci_evidence_fail "expected exactly one live artifact named $name, found $count"

    jq -r --arg name "$name" --arg sha "$COMMIT" --argjson run_id "$RUN_ID" '
        .artifacts[] | select(.name == $name and .expired == false) |
        select(.workflow_run.head_sha == $sha and .workflow_run.id == $run_id) |
        [.id, .name, .digest] | @tsv
    ' "$JSON" > "$records_one"
    [ "$(wc -l < "$records_one" | tr -d '[:space:]')" = 1 ] ||
        ci_evidence_fail "artifact $name does not belong to run $RUN_ID at commit $COMMIT"

    IFS=$(printf '\t') read -r id actual_name digest < "$records_one"
    rm -f -- "$records_one"

    ci_evidence_validate_number "$id"
    [ "$actual_name" = "$name" ] ||
        ci_evidence_fail "artifact name changed while indexing: $name"
    printf '%s\n' "$digest" | grep -Eq '^sha256:[0-9a-f]{64}$' ||
        ci_evidence_fail "artifact $name has no canonical SHA-256 digest"

    printf '%s|%s|%s\n' "$id" "$actual_name" "$digest" >> "$records"
done

record_count=$(wc -l < "$records" | tr -d '[:space:]')
[ "$record_count" = "$CI_TESTS_EVIDENCE_COUNT" ] ||
    ci_evidence_fail 'artifact index is incomplete'
[ "$(cut -d'|' -f1 "$records" | LC_ALL=C sort -u | wc -l | tr -d '[:space:]')" = \
    "$CI_TESTS_EVIDENCE_COUNT" ] ||
    ci_evidence_fail 'artifact IDs are not unique'
[ "$(cut -d'|' -f2 "$records" | LC_ALL=C sort -u | wc -l | tr -d '[:space:]')" = \
    "$CI_TESTS_EVIDENCE_COUNT" ] ||
    ci_evidence_fail 'artifact names are not unique'
LC_ALL=C sort -t'|' -k2,2 -o "$records" "$records"

ci_evidence_prepare_directory "$OUTPUT" 'tests evidence index directory'
{
    printf 'format=1\n'
    printf 'source_repository=%s\n' "$REPOSITORY"
    printf 'source_commit=%s\n' "$COMMIT"
    printf 'run_id=%s\n' "$RUN_ID"
    printf 'run_attempt=%s\n' "$RUN_ATTEMPT"
    printf 'artifact_count=%s\n' "$CI_TESTS_EVIDENCE_COUNT"
    sed 's/^/artifact=/' "$records"
} | cup_path_write_file "$CI_EVIDENCE_STAGING/index.txt" 0644 replace

ci_evidence_require_canonical_text "$CI_EVIDENCE_STAGING/index.txt" 65536
ci_evidence_commit_directory
trap - EXIT HUP INT TERM
cleanup_records

printf '%s\n' "$OUTPUT"
