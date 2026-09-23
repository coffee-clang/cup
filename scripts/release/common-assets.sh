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
: "${RELEASE_RUN_ID:?RELEASE_RUN_ID is required}"
: "${CATALOG_SOURCE:?CATALOG_SOURCE is required}"
validate_release_provenance_inputs "$SOURCE_REPOSITORY" \
    "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$RELEASE_RUN_ID"

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

PUBLIC=$OUTPUT_STAGING/public
cup_path_prepare_child_directory "$BUILD_ROOT" "$PUBLIC" "release public directory"
require_nonempty_file "$CATALOG_SOURCE"
[ "$(sed -n '1p' "$CATALOG_SOURCE")" = 'format=1' ] ||
    fail 'published catalog snapshot is not format 1'
catalog_revision=$(sed -n '2s/^revision=//p' "$CATALOG_SOURCE")
case "$catalog_revision" in ''|*[!0-9]*) fail 'published catalog snapshot has an invalid revision' ;; esac
[ "$(sed -n '3p' "$CATALOG_SOURCE")" = \
    'update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg' ] ||
    fail 'published catalog snapshot has an unexpected update URL'
cup_path_copy_file "$CATALOG_SOURCE" "$PUBLIC/catalog.cfg" 0644 replace
cup_path_copy_file "$PROJECT_ROOT/LICENSE" "$PUBLIC/LICENSE" 0644 replace
cup_path_copy_file "$PROJECT_ROOT/scripts/dependencies/THIRD_PARTY_NOTICES.txt" "$PUBLIC/THIRD_PARTY_NOTICES.txt" 0644 replace
prepare_installer "$PROJECT_ROOT/scripts/install/install.sh" "$PUBLIC/install.sh" 0755
prepare_installer "$PROJECT_ROOT/scripts/install/install.ps1" "$PUBLIC/install.ps1" 0644

cat <<PROVENANCE | cup_path_write_file "$PUBLIC/provenance.txt" 0644 replace
format=4
version=$VERSION
source_repository=$SOURCE_REPOSITORY
source_commit=$SHA
tests_run_id=$TESTS_RUN_ID
tests_run_attempt=$TESTS_RUN_ATTEMPT
release_run_id=$RELEASE_RUN_ID
PROVENANCE

validate_provenance_file "$PUBLIC/provenance.txt" "$SOURCE_REPOSITORY" \
    "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$RELEASE_RUN_ID"
grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" "$PUBLIC/install.sh" >/dev/null
grep -F "CUP_RELEASE_TAG=\"$TAG\"" "$PUBLIC/install.sh" >/dev/null
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" "$PUBLIC/install.sh" >/dev/null
grep -F "\$ReleaseVersion = \"$VERSION\"" "$PUBLIC/install.ps1" >/dev/null
grep -F "\$ReleaseTag = \"$TAG\"" "$PUBLIC/install.ps1" >/dev/null
grep -F "\$ReleaseCommit = \"$SHA\"" "$PUBLIC/install.ps1" >/dev/null
! grep -E '@CUP_RELEASE_(VERSION|TAG|COMMIT)@' "$PUBLIC/install.sh" "$PUBLIC/install.ps1" >/dev/null
# shellcheck disable=SC2086
validate_exact_directory_files "$PUBLIC" $(release_common_public_assets)

commit_output_staging "$OUTPUT"
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
