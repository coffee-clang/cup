#!/bin/sh

# Verifies that a restored dependency prefix matches one run-bound artifact.
set -eu
LC_ALL=C
LANG=C
export LC_ALL LANG

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"

fail() {
    printf 'Dependency evidence: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf '%s\n' \
        "Usage: $0 <evidence> <target> <platform> <profile> <prefix>" \
        '       [<repository> <commit> <run-id> <run-attempt> <artifact-name>]' >&2
    exit 2
}

parse_arguments() {
    case "$#" in
        5)
            EVIDENCE=$1
            TARGET=$2
            PLATFORM=$3
            PROFILE=$4
            PREFIX=$5
            REPOSITORY=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}
            COMMIT=${GITHUB_SHA:?GITHUB_SHA is required}
            RUN_ID=${GITHUB_RUN_ID:?GITHUB_RUN_ID is required}
            RUN_ATTEMPT=${GITHUB_RUN_ATTEMPT:?GITHUB_RUN_ATTEMPT is required}
            ARTIFACT_NAME=cup-dependency-evidence-$TARGET-attempt-$RUN_ATTEMPT
            ;;
        10)
            EVIDENCE=$1
            TARGET=$2
            PLATFORM=$3
            PROFILE=$4
            PREFIX=$5
            REPOSITORY=$6
            COMMIT=$7
            RUN_ID=$8
            RUN_ATTEMPT=$9
            shift 9
            ARTIFACT_NAME=$1
            ;;
        *)
            usage
            ;;
    esac
}

validate_expected_identity() {
    ci_evidence_validate_repository "$REPOSITORY" || fail 'invalid expected repository'
    ci_evidence_validate_sha "$COMMIT" || fail 'invalid expected commit'
    ci_evidence_validate_number "$RUN_ID" || fail 'invalid expected run ID'
    ci_evidence_validate_number "$RUN_ATTEMPT" || fail 'invalid expected run attempt'
    ci_evidence_validate_artifact_name "$ARTIFACT_NAME" ||
        fail 'invalid expected artifact name'
    ci_evidence_validate_slug "$TARGET" target || fail 'invalid expected target'
    ci_evidence_validate_slug "$PLATFORM" platform || fail 'invalid expected platform'
    ci_evidence_validate_slug "$PROFILE" profile || fail 'invalid expected profile'
}

verify_evidence_header() {
    expected_header=$(cat <<EOF_HEADER
format=2
version=$VERSION
source_repository=$REPOSITORY
source_commit=$COMMIT
run_id=$RUN_ID
run_attempt=$RUN_ATTEMPT
artifact_name=$ARTIFACT_NAME
target=$TARGET
platform=$PLATFORM
profile=$PROFILE
EOF_HEADER
)
    actual_header=$(sed -n '1,10p' "$EVIDENCE")
    [ "$actual_header" = "$expected_header" ] ||
        fail 'evidence identity or schema does not match'

    cache_line=$(sed -n '11p' "$EVIDENCE")
    case "$cache_line" in
        cache_key=*)
            EXPECTED_CACHE_KEY=${cache_line#cache_key=}
            ;;
        *)
            fail 'evidence cache key is missing'
            ;;
    esac
    [ -n "$EXPECTED_CACHE_KEY" ] || fail 'evidence cache key is empty'
}

verify_dependency_prefix() {
    command -v cmp >/dev/null 2>&1 || fail 'required tool is unavailable: cmp'

    CUP_DEPENDENCY_PROFILE=$PROFILE \
        "$PROJECT_ROOT/scripts/dependencies/verify.sh" \
        "$PLATFORM" "$PREFIX" >/dev/null

    actual_key=$(CUP_DEPENDENCY_PROFILE=$PROFILE \
        "$PROJECT_ROOT/scripts/dependencies/verify.sh" \
        "$PLATFORM" --print-cache-key)
    [ "$actual_key" = "$EXPECTED_CACHE_KEY" ] ||
        fail 'restored prefix cache key differs from evidence'

    metadata=$PREFIX/.cup-dependencies
    ci_evidence_require_canonical_text "$metadata" ||
        fail 'dependency prefix metadata is missing or non-canonical'

    temporary=$(mktemp "${TMPDIR:-/tmp}/cup-dependency-evidence.XXXXXX")
    cleanup_temporary() {
        rm -f -- "$temporary"
    }
    trap cleanup_temporary EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    sed -n '12,$p' "$EVIDENCE" > "$temporary"
    cmp -s "$temporary" "$metadata" ||
        fail 'dependency prefix metadata differs from evidence'
}

parse_arguments "$@"

EVIDENCE=$(ci_evidence_resolve_path "$PROJECT_ROOT" "$EVIDENCE")
PREFIX=$(ci_evidence_resolve_path "$PROJECT_ROOT" "$PREFIX")
VERSION=$("$PROJECT_ROOT/scripts/version.sh" base)

ci_evidence_require_canonical_text "$EVIDENCE" || fail 'evidence is not canonical'
validate_expected_identity
verify_evidence_header
verify_dependency_prefix

printf 'Dependency evidence verified for %s from run %s attempt %s.\n' \
    "$TARGET" "$RUN_ID" "$RUN_ATTEMPT"
