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
    binary="cup-$PLATFORM.exe"
else
    cp "build/$PLATFORM/static/bin/cup" "dist/$PLATFORM/cup-$PLATFORM"
    chmod +x "dist/$PLATFORM/cup-$PLATFORM"
    binary="cup-$PLATFORM"
fi

checksum=$(checksum_command)
(cd "dist/$PLATFORM" && $checksum "$binary" > "SHA256SUMS.$PLATFORM")

if [ "$FAMILY" != windows ]; then
    "dist/$PLATFORM/$binary" --version
    "dist/$PLATFORM/$binary" help >/dev/null
fi
