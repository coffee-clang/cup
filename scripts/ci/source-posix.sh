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

if [ "${CUP_CI_ENVIRONMENT_PREPARED:-0}" != 1 ]; then
    FAMILY="$family" PLATFORM="$platform" "$(dirname "$0")/prepare-posix.sh" source
fi

make PLATFORM="$platform" deps

PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" make test
make PLATFORM="$platform" check-binary

# Linux releases and native integration tests are GCC-owned. A second x64 pass
# compiles the complete application and all C unit tests with Clang, so compiler
# diversity is an exercised contract rather than an unused package installation.
if [ "$platform" = linux-x64 ]; then
    make clean
    make PLATFORM="$platform" CC=clang
    PLATFORM="$platform" CUP_TEST_PLATFORM="$platform" make CC=clang test-unit
    make PLATFORM="$platform" CC=clang check-binary
fi

# The network portability smoke test is intentionally a Linux x64 pilot. It
# stays outside the ordinary local test target because it generates certificates,
# starts local servers and builds one isolated static release.
if [ "$platform" = linux-x64 ]; then
    make clean
    PLATFORM="$platform" make test-portability-linux
fi
