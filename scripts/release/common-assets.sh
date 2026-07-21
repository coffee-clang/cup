#!/usr/bin/env sh

# Purpose: Builds common candidate metadata, manifest, installers, uninstall helpers and checksums.
# Outputs are staged below dist/common for Actions artifact upload.
set -eu

. "$(dirname "$0")/common.sh"

# Validate workflow provenance before generating any candidate metadata.
: "${SOURCE_REPOSITORY:?SOURCE_REPOSITORY is required}"
: "${TESTS_RUN_ID:?TESTS_RUN_ID is required}"
: "${TESTS_RUN_ATTEMPT:?TESTS_RUN_ATTEMPT is required}"

printf '%s\n' "$SOURCE_REPOSITORY" |
    grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' ||
    fail "invalid SOURCE_REPOSITORY: $SOURCE_REPOSITORY"
printf '%s\n' "$TESTS_RUN_ID" | grep -Eq '^[1-9][0-9]*$' ||
    fail "invalid TESTS_RUN_ID: $TESTS_RUN_ID"
printf '%s\n' "$TESTS_RUN_ATTEMPT" | grep -Eq '^[1-9][0-9]*$' ||
    fail "invalid TESTS_RUN_ATTEMPT: $TESTS_RUN_ATTEMPT"

# Build the common immutable assets from the same locally tagged source revision.
create_local_release_tag
mkdir -p build/release-common/generated dist/common
CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh generate build/release-common/generated

test "$(sed -n 's/^version=//p' build/release-common/generated/release.txt)" = "$VERSION"
test "$(sed -n 's/^commit=//p' build/release-common/generated/release.txt)" = "$SHA"

cp build/release-common/generated/release.txt dist/common/release.txt
cp config/packages.cfg dist/common/packages.cfg
cp config/install.cfg dist/common/install.cfg
cp THIRD_PARTY_DEPENDENCIES.txt dist/common/THIRD_PARTY_DEPENDENCIES.txt
cp scripts/install/uninstall-cup.sh dist/common/uninstall.sh
cp scripts/install/uninstall-cup-windows.ps1 dist/common/uninstall.ps1
prepare_installer scripts/install/install-cup.sh dist/common/install.sh
prepare_installer scripts/install/install-cup-windows.ps1 dist/common/install.ps1
chmod +x dist/common/install.sh dist/common/uninstall.sh
# Internal candidate identity and public source-workflow provenance.
cat > dist/common/candidate.env <<EOF_CANDIDATE
VERSION=$VERSION
TAG=$TAG
SHA=$SHA
EOF_CANDIDATE
cat > dist/common/provenance.txt <<EOF_PROVENANCE
format=1
version=$VERSION
source_repository=$SOURCE_REPOSITORY
source_commit=$SHA
tests_run_id=$TESTS_RUN_ID
tests_run_attempt=$TESTS_RUN_ATTEMPT
EOF_PROVENANCE

# The installer checksum file intentionally covers only assets it consumes.
checksum=$(checksum_command)
(cd dist/common && $checksum packages.cfg install.cfg install.sh install.ps1 > SHA256SUMS.common)

grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" dist/common/install.sh
grep -F "CUP_RELEASE_TAG=\"$TAG\"" dist/common/install.sh
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" dist/common/install.sh
grep -F "\$ReleaseVersion = \"$VERSION\"" dist/common/install.ps1
grep -F "\$ReleaseTag = \"$TAG\"" dist/common/install.ps1
grep -F "\$ReleaseCommit = \"$SHA\"" dist/common/install.ps1
! grep -R '@CUP_RELEASE_' dist/common
