#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

: "${GH_REPO:?GH_REPO is required}"
: "${GH_TOKEN:?GH_TOKEN is required}"

dist=${1:-dist}
validate_release_inputs

if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
    fail "release $TAG already exists; refusing to replace it"
fi
if git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1; then
    fail "remote tag $TAG already exists; refusing to publish"
fi

for asset in \
    cup-linux-x64 cup-linux-arm64 cup-macos-x64 cup-macos-arm64 \
    cup-windows-x64.exe packages.cfg release.txt install.sh install.ps1 \
    uninstall.sh uninstall.ps1 SHA256SUMS.common \
    SHA256SUMS.linux-x64 SHA256SUMS.linux-arm64 \
    SHA256SUMS.macos-x64 SHA256SUMS.macos-arm64 \
    SHA256SUMS.windows-x64; do
    [ -f "$dist/$asset" ] || fail "missing asset: $asset"
done

test "$(sed -n 's/^format=//p' "$dist/release.txt")" = 1
test "$(sed -n 's/^version=//p' "$dist/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$dist/release.txt")" = "$SHA"
test "$(wc -l < "$dist/release.txt" | tr -d '[:space:]')" = 3

grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" "$dist/install.sh"
grep -F "CUP_RELEASE_TAG=\"$TAG\"" "$dist/install.sh"
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" "$dist/install.sh"
grep -F "\$ReleaseVersion = \"$VERSION\"" "$dist/install.ps1"
grep -F "\$ReleaseTag = \"$TAG\"" "$dist/install.ps1"
grep -F "\$ReleaseCommit = \"$SHA\"" "$dist/install.ps1"
! grep -R '@CUP_RELEASE_' "$dist"

verify_checksums "$dist" SHA256SUMS.common
for platform in linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64; do
    verify_checksums "$dist" "SHA256SUMS.$platform"
done

gh release create "$TAG" "$dist"/* --repo "$GH_REPO" \
    --target "$SHA" --title "cup $VERSION" --generate-notes --draft
gh release edit "$TAG" --repo "$GH_REPO" --draft=false --latest
