#!/bin/sh

# Installs and verifies one native POSIX CI tool profile.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG

profile=${1:?CI profile is required}
family=${FAMILY:?FAMILY is required}
platform=${PLATFORM:?PLATFORM is required}

fail() {
    printf 'CI environment: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool was not prepared: $1"
}

require_one_of() {
    requirement=$1
    shift
    for candidate in "$@"; do
        command -v "$candidate" >/dev/null 2>&1 && return 0
    done
    fail "$requirement requires one of: $*"
}

validate_native_platform() {
    host_system=$(uname -s)
    host_machine=$(uname -m)
    case "$family:$platform:$host_system:$host_machine" in
        linux:linux-x64:Linux:x86_64|linux:linux-x64:Linux:amd64) ;;
        linux:linux-arm64:Linux:aarch64|linux:linux-arm64:Linux:arm64) ;;
        macos:macos-x64:Darwin:x86_64|macos:macos-x64:Darwin:amd64) ;;
        macos:macos-arm64:Darwin:arm64|macos:macos-arm64:Darwin:aarch64) ;;
        *) fail "PLATFORM '$platform' and FAMILY '$family' do not match host $host_system/$host_machine" ;;
    esac
}

verify_common_tools() {
    for tool in awk basename cat chmod cksum cmp cp curl cut date dirname file git grep \
            head id make mkdir mktemp mv od perl pkg-config rm sed sort stat tar tr uname wc xz; do
        require_tool "$tool"
    done
    require_one_of 'SHA-256 verification' sha256sum shasum
}

verify_native_toolchain() {
    case "$family" in
        linux)
            for tool in ar gcc ranlib; do
                require_tool "$tool"
            done
            ;;
        macos)
            for tool in ar clang ranlib xcrun; do
                require_tool "$tool"
            done
            ;;
    esac
}

verify_binary_tools() {
    require_tool strings
    case "$family" in
        linux)
            require_tool readelf
            ;;
        macos)
            require_tool lipo
            require_tool otool
            ;;
    esac
}

verify_llvm_tool() {
    xcrun --sdk macosx --find "$1" >/dev/null 2>&1 || fail "required Apple LLVM tool was not prepared: $1"
}

verify_profile_tools() {
    verify_common_tools
    verify_native_toolchain
    case "$profile:$family" in
        dependencies:*) ;;
        source:linux)
            require_tool openssl
            require_one_of 'bounded source tests' timeout gtimeout
            verify_binary_tools
            [ "$platform" != linux-x64 ] || require_tool clang
            ;;
        source:macos)
            require_one_of 'bounded source tests' timeout gtimeout
            verify_binary_tools
            ;;
        coverage:linux)
            require_tool gcov
            require_tool gcovr
            require_one_of 'bounded coverage tests' timeout gtimeout
            verify_binary_tools
            ;;
        coverage:macos)
            require_tool gcovr
            verify_llvm_tool llvm-cov
            verify_llvm_tool llvm-profdata
            require_one_of 'bounded coverage tests' timeout gtimeout
            verify_binary_tools
            ;;
        sanitizers:linux|sanitizers:macos)
            require_tool clang
            require_one_of 'bounded sanitizer tests' timeout gtimeout
            verify_binary_tools
            ;;
        debug:linux)
            verify_binary_tools
            require_tool objcopy
            ;;
        debug:macos)
            verify_binary_tools
            require_tool dsymutil
            require_tool dwarfdump
            ;;
        release:linux)
            verify_binary_tools
            require_tool objcopy
            require_tool strip
            require_one_of 'bounded release tests' timeout gtimeout
            ;;
        release:macos)
            verify_binary_tools
            require_tool dsymutil
            require_tool dwarfdump
            require_tool strip
            require_one_of 'bounded release tests' timeout gtimeout
            ;;
        *) fail "unsupported profile/family combination: $profile/$family" ;;
    esac
}

report_packages() {
    case "$family" in
        linux)
            command -v dpkg-query >/dev/null 2>&1 || return 0
            # shellcheck disable=SC2086
            dpkg-query -W -f='${Package}=${Version}\n' $packages 2>/dev/null || true
            ;;
        macos)
            # shellcheck disable=SC2086
            brew list --versions $packages 2>/dev/null || true
            ;;
    esac
}

case "$profile" in
    dependencies|source|coverage|sanitizers|debug|release)
        ;;
    *)
        fail "unsupported profile: $profile"
        ;;
esac
case "$family" in
    linux|macos)
        ;;
    *)
        fail "unsupported POSIX family: $family"
        ;;
esac
validate_native_platform

case "$family" in
    linux)
        packages='build-essential ca-certificates curl file git make openssl perl pkg-config tar xz-utils'
        case "$profile" in
            source) [ "$platform" != linux-x64 ] || packages="$packages clang" ;;
            coverage) packages="$packages gcovr" ;;
            sanitizers) packages="$packages clang llvm" ;;
        esac
        sudo apt-get update
        # shellcheck disable=SC2086
        sudo apt-get install -y --no-install-recommends $packages
        ;;
    macos)
        brew untap aws/tap >/dev/null 2>&1 || true
        case "$profile" in
            source|sanitizers|release) packages='coreutils perl pkg-config xz' ;;
            coverage) packages='coreutils gcovr perl pkg-config xz' ;;
            dependencies|debug) packages='perl pkg-config xz' ;;
        esac
        for package in $packages; do
            brew list --formula "$package" >/dev/null 2>&1 || brew install "$package"
        done
        ;;
esac

report_packages
verify_profile_tools
