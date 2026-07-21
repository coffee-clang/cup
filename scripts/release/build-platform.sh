#!/usr/bin/env sh

# Purpose: Builds one static platform executable and its platform checksum asset.
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
        fail "PLATFORM '$PLATFORM' and FAMILY '$FAMILY' do not match host $host_system/$host_machine"
        ;;
esac

create_local_release_tag

# Prepare and verify the pinned third-party prefix on the native runner.
case "$FAMILY" in
    linux)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential ca-certificates curl file make perl pkg-config \
            tar xz-utils zlib1g-dev
        ;;
    macos)
        brew update
        for package in autoconf automake libtool pkg-config make curl perl xz; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
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

case "$FAMILY" in
    linux|macos) dependency_builder=scripts/dependencies/build-posix.sh ;;
    windows) dependency_builder=scripts/dependencies/build-windows.sh ;;
esac
JOBS="${JOBS:-4}" PLATFORM="$PLATFORM" bash "$dependency_builder"
CUP_OFFICIAL_BUILD=1 make PLATFORM="$PLATFORM" release
CUP_OFFICIAL_BUILD=1 make PLATFORM="$PLATFORM" \
    CUP_BUILD_CONFIGURATION=release check-binary

test "$(./scripts/version.sh base)" = "$VERSION"
mkdir -p "dist/$PLATFORM" "build/release-$PLATFORM/generated"
CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh generate "build/release-$PLATFORM/generated"

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

checksum=$(checksum_command)
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
