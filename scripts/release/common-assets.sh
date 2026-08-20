#!/bin/sh

# Produces the exact common release assets below the managed build root.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/common.sh"

validate_release_inputs
: "${SOURCE_REPOSITORY:?SOURCE_REPOSITORY is required}"
: "${TESTS_RUN_ID:?TESTS_RUN_ID is required}"
: "${TESTS_RUN_ATTEMPT:?TESTS_RUN_ATTEMPT is required}"
: "${TESTS_EVIDENCE_INDEX_SHA256:?TESTS_EVIDENCE_INDEX_SHA256 is required}"
: "${RELEASE_RUN_ID:?RELEASE_RUN_ID is required}"
: "${RELEASE_RUN_ATTEMPT:?RELEASE_RUN_ATTEMPT is required}"
validate_repository_identifier "$SOURCE_REPOSITORY" SOURCE_REPOSITORY
for value in "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$RELEASE_RUN_ID" "$RELEASE_RUN_ATTEMPT"; do
    printf '%s\n' "$value" | grep -Eq '^[1-9][0-9]*$' || fail 'invalid provenance run identity'
done
printf '%s\n' "$TESTS_EVIDENCE_INDEX_SHA256" | grep -Eq '^[0-9a-f]{64}$' ||
    fail 'invalid TESTS_EVIDENCE_INDEX_SHA256'

BUILD_ROOT=${CUP_BUILD_ROOT:-$PROJECT_ROOT/build}
OUTPUT=${1:-$BUILD_ROOT/release/common}
case "$OUTPUT" in /*) ;; *) OUTPUT=$PROJECT_ROOT/$OUTPUT ;; esac
prepare_output_staging "$OUTPUT" "$BUILD_ROOT"
cleanup_common() {
    [ -z "${OUTPUT_STAGING:-}" ] || cup_path_remove_child_tree "$BUILD_ROOT" "$OUTPUT_STAGING" 'common release staging'
}
trap cleanup_common EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

GENERATED=$OUTPUT_STAGING/generated
PUBLIC=$OUTPUT_STAGING/public
cup_path_prepare_child_directory "$BUILD_ROOT" "$GENERATED" "release generated directory"
cup_path_prepare_child_directory "$BUILD_ROOT" "$PUBLIC" "release public directory"
CUP_BUILD_ROOT=$BUILD_ROOT CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
CUP_RELEASE_VERSION=$VERSION CUP_RELEASE_TAG=$TAG CUP_RELEASE_COMMIT=$SHA \
    sh "$PROJECT_ROOT/scripts/version.sh" generate "$GENERATED"
validate_release_file "$GENERATED/release.txt"

cup_path_copy_file "$GENERATED/release.txt" "$PUBLIC/release.txt" 0644 replace
cup_path_copy_file "$PROJECT_ROOT/config/packages.cfg" "$PUBLIC/packages.cfg" 0644 replace
cup_path_copy_file "$PROJECT_ROOT/config/install.cfg" "$PUBLIC/install.cfg" 0644 replace
cup_path_copy_file \
    "$PROJECT_ROOT/scripts/dependencies/THIRD_PARTY_NOTICES.txt" \
    "$PUBLIC/THIRD_PARTY_NOTICES.txt" 0644 replace
cup_path_copy_file "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" "$PUBLIC/uninstall.sh" 0755 replace
cup_path_copy_file "$PROJECT_ROOT/scripts/install/uninstall-cup-windows.ps1" "$PUBLIC/uninstall.ps1" 0644 replace
prepare_installer "$PROJECT_ROOT/scripts/install/install-cup.sh" "$PUBLIC/install.sh" 0755
prepare_installer "$PROJECT_ROOT/scripts/install/install-cup-windows.ps1" "$PUBLIC/install.ps1" 0644
chmod 0755 "$PUBLIC/install.sh" "$PUBLIC/uninstall.sh"
chmod 0644 "$PUBLIC/packages.cfg" "$PUBLIC/install.cfg" "$PUBLIC/release.txt" \
    "$PUBLIC/THIRD_PARTY_NOTICES.txt" "$PUBLIC/install.ps1" "$PUBLIC/uninstall.ps1"

cat <<PROVENANCE | cup_path_write_file "$PUBLIC/provenance.txt" 0644 replace
format=3
version=$VERSION
source_repository=$SOURCE_REPOSITORY
source_commit=$SHA
tests_run_id=$TESTS_RUN_ID
tests_run_attempt=$TESTS_RUN_ATTEMPT
tests_evidence_index_sha256=$TESTS_EVIDENCE_INDEX_SHA256
release_run_id=$RELEASE_RUN_ID
release_run_attempt=$RELEASE_RUN_ATTEMPT
PROVENANCE
chmod 0644 "$PUBLIC/provenance.txt"

{
    for asset in packages.cfg install.cfg install.sh install.ps1; do
        printf '%s  %s\n' "$(hash_file "$PUBLIC/$asset")" "$asset"
    done
} > "$PUBLIC/SHA256SUMS.common"
chmod 0644 "$PUBLIC/SHA256SUMS.common"

validate_provenance_file "$PUBLIC/provenance.txt" "$SOURCE_REPOSITORY" \
    "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$TESTS_EVIDENCE_INDEX_SHA256" \
    "$RELEASE_RUN_ID" "$RELEASE_RUN_ATTEMPT"
verify_checksum_file_exact "$PUBLIC" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" "$PUBLIC/install.sh" >/dev/null
grep -F "CUP_RELEASE_TAG=\"$TAG\"" "$PUBLIC/install.sh" >/dev/null
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" "$PUBLIC/install.sh" >/dev/null
grep -F "\$ReleaseVersion = \"$VERSION\"" "$PUBLIC/install.ps1" >/dev/null
grep -F "\$ReleaseTag = \"$TAG\"" "$PUBLIC/install.ps1" >/dev/null
grep -F "\$ReleaseCommit = \"$SHA\"" "$PUBLIC/install.ps1" >/dev/null
! grep -E '@CUP_RELEASE_(VERSION|TAG|COMMIT)@' "$PUBLIC/install.sh" "$PUBLIC/install.ps1" >/dev/null
validate_exact_directory_files "$PUBLIC" \
    THIRD_PARTY_NOTICES.txt SHA256SUMS.common install.cfg install.ps1 install.sh \
    packages.cfg provenance.txt release.txt uninstall.ps1 uninstall.sh

cup_path_remove_child_tree "$BUILD_ROOT" "$GENERATED" 'release generated directory'
commit_output_staging "$OUTPUT"
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
