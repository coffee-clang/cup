#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

create_local_release_tag
mkdir -p build/release-common/generated dist/common
CUP_RELEASE_BUILD=1 ./scripts/version.sh generate build/release-common/generated

test "$(sed -n 's/^version=//p' build/release-common/generated/release.txt)" = "$VERSION"
test "$(sed -n 's/^commit=//p' build/release-common/generated/release.txt)" = "$SHA"

cp build/release-common/generated/release.txt dist/common/release.txt
cp config/packages.cfg dist/common/packages.cfg
cp scripts/install/uninstall-cup.sh dist/common/uninstall.sh
cp scripts/install/uninstall-cup-windows.ps1 dist/common/uninstall.ps1
prepare_installer scripts/install/install-cup.sh dist/common/install.sh
prepare_installer scripts/install/install-cup-windows.ps1 dist/common/install.ps1
chmod +x dist/common/install.sh dist/common/uninstall.sh
cat > dist/common/candidate.env <<EOF_CANDIDATE
VERSION=$VERSION
TAG=$TAG
SHA=$SHA
EOF_CANDIDATE

checksum=$(checksum_command)
(
    cd dist/common
    $checksum \
        candidate.env packages.cfg release.txt install.sh install.ps1 \
        uninstall.sh uninstall.ps1 > SHA256SUMS.common
)

grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" dist/common/install.sh
grep -F "CUP_RELEASE_TAG=\"$TAG\"" dist/common/install.sh
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" dist/common/install.sh
grep -F "\$ReleaseVersion = \"$VERSION\"" dist/common/install.ps1
grep -F "\$ReleaseTag = \"$TAG\"" dist/common/install.ps1
grep -F "\$ReleaseCommit = \"$SHA\"" dist/common/install.ps1
! grep -R '@CUP_RELEASE_' dist/common
