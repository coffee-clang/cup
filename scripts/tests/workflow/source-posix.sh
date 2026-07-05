#!/usr/bin/env sh
set -eu

platform=${PLATFORM:-linux-x64}
family=${FAMILY:-linux}

case "$family" in
    linux)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential clang git libarchive-dev libcurl4-openssl-dev make
        CUP_DEPS_SCOPE=development bash scripts/bootstrap/bootstrap-linux-deps.sh
        ;;
    macos)
        for package in curl libarchive pkg-config; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
        CUP_DEPS_SCOPE=development bash scripts/bootstrap/bootstrap-macos-deps.sh
        ;;
    *)
        echo "Unsupported source-test family: $family" >&2
        exit 1
        ;;
esac

PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" make test
