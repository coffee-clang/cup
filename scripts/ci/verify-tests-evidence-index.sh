#!/bin/sh

# Verifies and queries a canonical tests evidence artifact index.
set -eu
LC_ALL=C
LANG=C
export LC_ALL LANG

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"
. "$SCRIPT_DIR/tests-evidence-artifacts.sh"

[ "$#" -ge 5 ] && [ "$#" -le 7 ] || {
    printf 'Usage: %s <index> <repository> <commit> <run-id> <run-attempt> [--artifact-id|--artifact-digest <name>]\n' \
        "$0" >&2
    exit 2
}

INDEX=$1
REPOSITORY=$2
COMMIT=$3
RUN_ID=$4
RUN_ATTEMPT=$5
MODE=${6:-verify}
REQUESTED=${7:-}
INDEX=$(ci_evidence_resolve_path "$(pwd -P)" "$INDEX")

fail() {
    printf 'Tests evidence index: %s\n' "$*" >&2
    exit 1
}

ci_evidence_require_canonical_text "$INDEX" 65536 || fail 'index is not canonical'
ci_evidence_validate_repository "$REPOSITORY" || fail 'invalid repository'
ci_evidence_validate_sha "$COMMIT" || fail 'invalid commit'
ci_evidence_validate_number "$RUN_ID" || fail 'invalid run ID'
ci_evidence_validate_number "$RUN_ATTEMPT" || fail 'invalid run attempt'

expected_header=$(cat <<EOF_HEADER
format=1
source_repository=$REPOSITORY
source_commit=$COMMIT
run_id=$RUN_ID
run_attempt=$RUN_ATTEMPT
artifact_count=$CI_TESTS_EVIDENCE_COUNT
EOF_HEADER
)
[ "$(sed -n '1,6p' "$INDEX")" = "$expected_header" ] ||
    fail 'header identity or schema does not match'

total_lines=$((CI_TESTS_EVIDENCE_COUNT + 6))
[ "$(wc -l < "$INDEX" | tr -d '[:space:]')" = "$total_lines" ] ||
    fail 'index line count does not match'

records=$(mktemp "${TMPDIR:-/tmp}/cup-tests-index-verify.XXXXXX")
expected=$(mktemp "${TMPDIR:-/tmp}/cup-tests-index-names.XXXXXX")

cleanup() {
    rm -f -- "$records" "$expected"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
sed -n "7,${total_lines}p" "$INDEX" | sed 's/^artifact=//' > "$records"
ci_tests_evidence_names "$RUN_ATTEMPT" | LC_ALL=C sort > "$expected"

[ "$(cut -d'|' -f2 "$records")" = "$(cat "$expected")" ] ||
    fail 'artifact exact-set or order does not match'
awk -F'|' '
    NF != 3 { exit 1 }
    $1 !~ /^[1-9][0-9]*$/ { exit 1 }
    $2 !~ /^[A-Za-z0-9][A-Za-z0-9_.-]{0,199}$/ { exit 1 }
    $3 !~ /^sha256:[0-9a-f]{64}$/ { exit 1 }
' "$records" || fail 'artifact record is malformed'

[ "$(cut -d'|' -f1 "$records" | sort -u | wc -l | tr -d '[:space:]')" = \
    "$CI_TESTS_EVIDENCE_COUNT" ] ||
    fail 'artifact IDs are not unique'
[ "$(cut -d'|' -f2 "$records" | sort -u | wc -l | tr -d '[:space:]')" = \
    "$CI_TESTS_EVIDENCE_COUNT" ] ||
    fail 'artifact names are not unique'

case "$MODE" in
    verify)
        [ "$#" -eq 5 ] || fail 'unexpected query argument'
        ;;
    --artifact-id|--artifact-digest)
        [ "$#" -eq 7 ] || fail 'artifact query requires a name'
        ci_evidence_validate_artifact_name "$REQUESTED" ||
            fail 'invalid requested artifact name'
        line=$(awk -F'|' -v name="$REQUESTED" '
            $2 == name { print; count++ }
            END { if (count != 1) exit 1 }
        ' "$records") || fail "artifact is not indexed: $REQUESTED"

        case "$MODE" in
            --artifact-id)
                printf '%s\n' "${line%%|*}"
                ;;
            --artifact-digest)
                printf '%s\n' "${line##*|}"
                ;;
        esac
        ;;
    *)
        fail "unsupported query mode: $MODE"
        ;;
esac
