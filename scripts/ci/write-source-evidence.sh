#!/bin/sh

# Builds one source-test evidence directory atomically and without replacement.
set -eu
LC_ALL=C
LANG=C
export LC_ALL LANG
umask 077
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/evidence-common.sh"

[ "$#" -eq 8 ] || {
    printf '%s\n' \
        "Usage: $0 <output-dir> <platform> <build-config> <release> <inspection>" \
        '       <repository> <run-id> <run-attempt>' >&2
    exit 2
}
OUTPUT=$1
PLATFORM=$2
BUILD_CONFIG=$3
RELEASE=$4
INSPECTION=$5
REPOSITORY=$6
RUN_ID=$7
RUN_ATTEMPT=$8
WORKING_DIRECTORY=$(pwd -P)

OUTPUT=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$OUTPUT")
BUILD_CONFIG=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$BUILD_CONFIG")
RELEASE=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$RELEASE")
INSPECTION=$(ci_evidence_resolve_path "$WORKING_DIRECTORY" "$INSPECTION")

ci_evidence_validate_repository "$REPOSITORY"
ci_evidence_validate_number "$RUN_ID"
ci_evidence_validate_number "$RUN_ATTEMPT"
ci_evidence_validate_slug "$PLATFORM" platform

ci_evidence_require_canonical_text "$BUILD_CONFIG"
ci_evidence_require_canonical_text "$RELEASE"
ci_evidence_require_canonical_text "$INSPECTION"

VERSION=$("$PROJECT_ROOT/scripts/version.sh" base)
RELEASE_VERSION=$(awk -F= 'NR == 2 && $1 == "version" && NF == 2 { print $2 }' "$RELEASE")
COMMIT=$(awk -F= 'NR == 3 && $1 == "commit" && NF == 2 { print $2 }' "$RELEASE")
release_schema=$(awk '
    NR == 1 && $0 == "format=1" { format_count++ }
    NR == 2 && $0 ~ /^version=[0-9]+\.[0-9]+\.[0-9]+$/ { version_count++ }
    NR == 3 && $0 ~ /^commit=[0-9a-f]{40}$/ { commit_count++ }
    END {
        if (NR == 3 && format_count == 1 && version_count == 1 && commit_count == 1) {
            print "valid"
        }
    }
' "$RELEASE")
[ "$release_schema" = valid ] || ci_evidence_fail 'release.txt has an unexpected schema'
[ "$RELEASE_VERSION" = "$VERSION" ] ||
    ci_evidence_fail 'release.txt version does not match VERSION'
ci_evidence_validate_sha "$COMMIT"
ARTIFACT_NAME=cup-source-evidence-$PLATFORM-attempt-$RUN_ATTEMPT
ci_evidence_validate_artifact_name "$ARTIFACT_NAME"

ci_evidence_prepare_directory "$OUTPUT" 'source evidence directory'
cleanup() {
    if [ -n "${CI_EVIDENCE_STAGING:-}" ]; then
        cup_path_remove_directory_tree \
            "$CI_EVIDENCE_STAGING" 'source evidence staging' \
            >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cup_path_copy_file "$BUILD_CONFIG" "$CI_EVIDENCE_STAGING/build-config.txt" 0644 replace
cup_path_copy_file "$RELEASE" "$CI_EVIDENCE_STAGING/release.txt" 0644 replace
cup_path_copy_file "$INSPECTION" "$CI_EVIDENCE_STAGING/binary-inspection.txt" 0644 replace

{
    printf 'format=1\n'
    printf 'version=%s\n' "$VERSION"
    printf 'source_repository=%s\n' "$REPOSITORY"
    printf 'source_commit=%s\n' "$COMMIT"
    printf 'run_id=%s\n' "$RUN_ID"
    printf 'run_attempt=%s\n' "$RUN_ATTEMPT"
    printf 'artifact_name=%s\n' "$ARTIFACT_NAME"
    printf 'platform=%s\n' "$PLATFORM"
    printf 'build_config_sha256=%s\n' "$(ci_evidence_sha256_file "$BUILD_CONFIG")"
    printf 'release_sha256=%s\n' "$(ci_evidence_sha256_file "$RELEASE")"
    printf 'binary_inspection_sha256=%s\n' "$(ci_evidence_sha256_file "$INSPECTION")"
} | cup_path_write_file "$CI_EVIDENCE_STAGING/evidence.txt" 0644 replace

for output_file in "$CI_EVIDENCE_STAGING"/*; do
    ci_evidence_require_canonical_text "$output_file"
done

ci_evidence_commit_directory
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
