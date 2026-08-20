#!/bin/sh

# Builds one dependency evidence directory atomically and without replacement.
set -eu
LC_ALL=C
LANG=C
export LC_ALL LANG
umask 077
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"

[ "$#" -eq 11 ] || {
    printf '%s\n' \
        "Usage: $0 <output-dir> <target> <platform> <profile> <prefix> <cache-key>" \
        '       <repository> <commit> <run-id> <run-attempt> <artifact-name>' >&2
    exit 2
}
OUTPUT=$1
TARGET=$2
PLATFORM=$3
PROFILE=$4
PREFIX=$5
CACHE_KEY=$6
REPOSITORY=$7
COMMIT=$8
RUN_ID=$9
shift 9
RUN_ATTEMPT=$1
ARTIFACT_NAME=$2
WORKING_DIRECTORY=$(pwd -P)

OUTPUT=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$OUTPUT")
PREFIX=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$PREFIX")

ci_evidence_validate_repository "$REPOSITORY"
ci_evidence_validate_sha "$COMMIT"
ci_evidence_validate_number "$RUN_ID"
ci_evidence_validate_number "$RUN_ATTEMPT"
ci_evidence_validate_artifact_name "$ARTIFACT_NAME"
ci_evidence_validate_slug "$TARGET" target
ci_evidence_validate_slug "$PLATFORM" platform
ci_evidence_validate_slug "$PROFILE" profile
[ -n "$CACHE_KEY" ] && [ "${#CACHE_KEY}" -le 1024 ] ||
    ci_evidence_fail 'invalid cache key'

METADATA=$PREFIX/.cup-dependencies
ci_evidence_require_canonical_text "$METADATA"
VERSION=$("$PROJECT_ROOT/scripts/version.sh" base)

ci_evidence_prepare_directory "$OUTPUT" 'dependency evidence directory'
cleanup() {
    if [ -n "${CI_EVIDENCE_STAGING:-}" ]; then
        cup_path_remove_directory_tree \
            "$CI_EVIDENCE_STAGING" 'dependency evidence staging' \
            >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

{
    printf 'format=2\n'
    printf 'version=%s\n' "$VERSION"
    printf 'source_repository=%s\n' "$REPOSITORY"
    printf 'source_commit=%s\n' "$COMMIT"
    printf 'run_id=%s\n' "$RUN_ID"
    printf 'run_attempt=%s\n' "$RUN_ATTEMPT"
    printf 'artifact_name=%s\n' "$ARTIFACT_NAME"
    printf 'target=%s\n' "$TARGET"
    printf 'platform=%s\n' "$PLATFORM"
    printf 'profile=%s\n' "$PROFILE"
    printf 'cache_key=%s\n' "$CACHE_KEY"
    cat "$METADATA"
} | cup_path_write_file "$CI_EVIDENCE_STAGING/evidence.txt" 0644 replace

ci_evidence_require_canonical_text "$CI_EVIDENCE_STAGING/evidence.txt"
ci_evidence_commit_directory
trap - EXIT HUP INT TERM

printf '%s\n' "$OUTPUT"
