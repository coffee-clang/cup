#!/usr/bin/env sh

# Purpose: Prepares one native POSIX source-test runner and invokes the
# repository test entry points.
# Inputs: PLATFORM and FAMILY supplied by tests.yml.
set -eu

platform=${PLATFORM:-linux-x64}
family=${FAMILY:-linux}
host_system=$(uname -s)
host_machine=$(uname -m)

fail() {
    printf 'source tests: %s\n' "$*" >&2
    exit 1
}

case "$family:$platform:$host_system:$host_machine" in
    linux:linux-x64:Linux:x86_64|linux:linux-x64:Linux:amd64) ;;
    linux:linux-arm64:Linux:aarch64|linux:linux-arm64:Linux:arm64) ;;
    macos:macos-x64:Darwin:x86_64|macos:macos-x64:Darwin:amd64) ;;
    macos:macos-arm64:Darwin:arm64|macos:macos-arm64:Darwin:aarch64) ;;
    *)
        fail "PLATFORM '$platform' and FAMILY '$family' do not match host $host_system/$host_machine"
        ;;
esac

case "$family" in
    linux)
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential ca-certificates clang curl git make openssl perl \
            pkg-config tar xz-utils
        make PLATFORM="$platform" deps
        ;;
    macos)
        for package in perl pkg-config xz; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
        make PLATFORM="$platform" deps
        ;;
    *) fail "unsupported source-test family: $family" ;;
esac

PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" make test
make PLATFORM="$platform" check-binary

# The network portability smoke test is intentionally a Linux x64 pilot. It
# stays outside the ordinary local test target because it generates certificates,
# starts local servers and builds one isolated static release.
if [ "$platform" = linux-x64 ]; then
    PLATFORM="$platform" make test-portability-linux
fi
