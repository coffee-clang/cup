#!/usr/bin/env sh

# Purpose: Installs the native POSIX tools required by one CI profile before
# the dependency identity is calculated and its cache is restored.
set -eu

profile=${1:?CI profile is required}
family=${FAMILY:?FAMILY is required}
platform=${PLATFORM:-}

fail() {
    printf 'CI environment: %s\n' "$*" >&2
    exit 1
}

case "$profile" in
    source | coverage | sanitizers | release) ;;
    *) fail "unsupported profile: $profile" ;;
esac

case "$family" in
    linux)
        packages='build-essential ca-certificates curl file git make perl pkg-config tar xz-utils'
        case "$profile" in
            source)
                # The primary Linux build uses GCC. Clang is installed only for
                # the x64 secondary compiler pass; OpenSSL is needed only by
                # the x64 network-portability fixture.
                case "$platform" in
                    linux-x64|'') packages="$packages clang openssl" ;;
                    linux-arm64) ;;
                    *) fail "unsupported Linux source platform: $platform" ;;
                esac
                ;;
            coverage) packages="$packages gcovr" ;;
            sanitizers)
                # Keep one sanitizer implementation across POSIX and Windows.
                packages="$packages clang llvm"
                ;;
            release) packages="$packages zlib1g-dev" ;;
        esac
        sudo apt-get update
        # Word splitting is intentional: packages is a controlled internal list.
        # shellcheck disable=SC2086
        sudo apt-get install -y --no-install-recommends $packages
        ;;
    macos)
        case "$profile" in
            source)
                packages='perl pkg-config xz'
                ;;
            coverage)
                packages='coreutils gcovr perl pkg-config xz'
                ;;
            sanitizers)
                packages='coreutils perl pkg-config xz'
                ;;
            release)
                brew update
                packages='autoconf automake curl libtool make perl pkg-config xz'
                ;;
        esac
        for package in $packages; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
        ;;
    *)
        fail "unsupported POSIX family: $family"
        ;;
esac
