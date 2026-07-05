#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${FAMILY:?FAMILY is required}"

create_local_release_tag

case "$FAMILY" in
    linux)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential ca-certificates curl file make perl pkg-config \
            tar xz-utils zlib1g-dev
        ;;
    macos)
        brew update
        for package in autoconf automake libtool pkg-config make curl xz; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
        ;;
    windows)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential ca-certificates curl file make mingw-w64 perl \
            pkg-config tar xz-utils zip
        ;;
    *)
        fail "unsupported release build family: $FAMILY"
        ;;
esac

bash "scripts/bootstrap/bootstrap-$FAMILY-deps.sh"
make PLATFORM="$PLATFORM" LINK_MODE=static BUILD_MODE=release RELEASE_BUILD=1

test "$(./scripts/version.sh base)" = "$VERSION"
mkdir -p "dist/$PLATFORM"

if [ "$PLATFORM" = windows-x64 ]; then
    cp "build/$PLATFORM/static/bin/cup.exe" "dist/$PLATFORM/cup-$PLATFORM.exe"
    cp scripts/install/uninstall-cup-windows.ps1 "dist/$PLATFORM/uninstall.ps1"
    binary="cup-$PLATFORM.exe"
    uninstall="uninstall.ps1"
else
    cp "build/$PLATFORM/static/bin/cup" "dist/$PLATFORM/cup-$PLATFORM"
    cp scripts/install/uninstall-cup.sh "dist/$PLATFORM/uninstall.sh"
    chmod +x "dist/$PLATFORM/cup-$PLATFORM" "dist/$PLATFORM/uninstall.sh"
    binary="cup-$PLATFORM"
    uninstall="uninstall.sh"
fi

cp "build/$PLATFORM/static/generated/release.txt" "dist/$PLATFORM/release.txt"
checksum=$(checksum_command)
(cd "dist/$PLATFORM" && $checksum "$binary" "$uninstall" release.txt > "SHA256SUMS.$PLATFORM")

if [ "$FAMILY" != windows ]; then
    "dist/$PLATFORM/$binary" --version
    "dist/$PLATFORM/$binary" help >/dev/null
fi
