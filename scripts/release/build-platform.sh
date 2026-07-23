#!/usr/bin/env sh

# Purpose: Builds one native release executable and its platform checksum asset.
# Inputs: PLATFORM, FAMILY and official release metadata.
set -eu

. "$(dirname "$0")/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${FAMILY:?FAMILY is required}"

# Reject accidental cross-host candidate builds before preparing dependencies.
host_system=$(uname -s)
host_machine=$(uname -m)
case "$FAMILY:$PLATFORM:$host_system:$host_machine" in
    linux:linux-x64:Linux:x86_64|linux:linux-x64:Linux:amd64) ;;
    linux:linux-arm64:Linux:aarch64|linux:linux-arm64:Linux:arm64) ;;
    macos:macos-x64:Darwin:x86_64|macos:macos-x64:Darwin:amd64) ;;
    macos:macos-arm64:Darwin:arm64|macos:macos-arm64:Darwin:aarch64) ;;
    windows:windows-x64:MINGW*:x86_64|windows:windows-x64:MSYS*:x86_64) ;;
    *)
        fail "PLATFORM '$PLATFORM' and FAMILY '$FAMILY' do not match \
host $host_system/$host_machine"
        ;;
esac

create_local_release_tag

# Prepare and verify the pinned third-party prefix on the native runner.
case "$FAMILY" in
    linux | macos)
        if [ "${CUP_CI_ENVIRONMENT_PREPARED:-0}" != 1 ]; then
            FAMILY="$FAMILY" scripts/ci/prepare-posix.sh release
        fi
        ;;
    windows)
        [ "${MSYSTEM:-}" = UCRT64 ] ||
            fail "Windows release builds require an MSYS2 UCRT64 shell"
        [ "${MINGW_PREFIX:-}" = /ucrt64 ] ||
            fail "Windows release builds require MINGW_PREFIX=/ucrt64"
        ;;
    *)
        fail "unsupported release build family: $FAMILY"
        ;;
esac

# The public release target owns idempotent dependency preparation. A restored
# cache is reused; a missing or incompatible prefix is rebuilt transactionally.
make check-ca-bundle
CUP_OFFICIAL_BUILD=1 make PLATFORM="$PLATFORM" release
CUP_OFFICIAL_BUILD=1 make PLATFORM="$PLATFORM" \
    CUP_BUILD_CONFIGURATION=release finalize-release
CUP_OFFICIAL_BUILD=1 make PLATFORM="$PLATFORM" \
    CUP_BUILD_CONFIGURATION=release check-binary

test "$(./scripts/version.sh base)" = "$VERSION"
mkdir -p "dist/$PLATFORM" "dist/symbols/$PLATFORM" "build/release-$PLATFORM/generated"
cp -R "build/$PLATFORM/release/symbols"/. "dist/symbols/$PLATFORM"/
CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
    ./scripts/version.sh generate "build/release-$PLATFORM/generated"

test "$(sed -n 's/^version=//p' build/release-$PLATFORM/generated/release.txt)" = "$VERSION"
test "$(sed -n 's/^commit=//p' build/release-$PLATFORM/generated/release.txt)" = "$SHA"

# Assemble public platform assets and their exact checksum set.
if [ "$PLATFORM" = windows-x64 ]; then
    cp "build/$PLATFORM/release/bin/cup.exe" "dist/$PLATFORM/cup-$PLATFORM.exe"
    binary="cup-$PLATFORM.exe"
    uninstall_source="scripts/install/uninstall-cup-windows.ps1"
    uninstall_asset="uninstall.ps1"
else
    cp "build/$PLATFORM/release/bin/cup" "dist/$PLATFORM/cup-$PLATFORM"
    chmod +x "dist/$PLATFORM/cup-$PLATFORM"
    binary="cup-$PLATFORM"
    uninstall_source="scripts/install/uninstall-cup.sh"
    uninstall_asset="uninstall.sh"
fi

binary_hash=$(hash_file "dist/$PLATFORM/$binary")
uninstall_hash=$(hash_file "$uninstall_source")
metadata_hash=$(hash_file "build/release-$PLATFORM/generated/release.txt")
{
    printf '%s  %s\n' "$binary_hash" "$binary"
    printf '%s  %s\n' "$uninstall_hash" "$uninstall_asset"
    printf '%s  release.txt\n' "$metadata_hash"
} > "dist/$PLATFORM/SHA256SUMS.$PLATFORM"

if [ "$FAMILY" != windows ]; then
    "dist/$PLATFORM/$binary" --version
    "dist/$PLATFORM/$binary" help >/dev/null
fi
