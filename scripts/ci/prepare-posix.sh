#!/usr/bin/env sh

# Purpose: Installs the native POSIX tools required by one CI profile before
# the dependency cache key is resolved and its prefix is restored.
set -eu

profile=${1:?CI profile is required}
family=${FAMILY:?FAMILY is required}
platform=${PLATFORM:-}

fail() {
    printf 'CI environment: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 ||
        fail "required tool was not prepared: $1"
}

require_one_of() {
    purpose=$1
    shift
    for candidate in "$@"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            return 0
        fi
    done
    fail "$purpose requires one of: $*"
}

require_llvm_tool() {
    llvm_tool=$1
    if command -v "$llvm_tool" >/dev/null 2>&1; then
        return 0
    fi
    command -v xcrun >/dev/null 2>&1 &&
        xcrun --find "$llvm_tool" >/dev/null 2>&1 && return 0
    fail "required LLVM tool was not prepared: $llvm_tool"
}

verify_common_tools() {
    # These commands form the controlled POSIX CI baseline used by the build,
    # dependency, test and release scripts. Hosted-runner presence alone is not
    # accepted as the contract: missing commands fail before the real work.
    for required_tool in awk basename cat chmod cmp cp curl cut date dirname \
            file git grep head make mkdir mktemp mv od perl pkg-config rm sed \
            sort tar tr uname wc xz; do
        require_tool "$required_tool"
    done
    require_one_of 'SHA-256 verification' sha256sum shasum
}

verify_native_toolchain() {
    case "$family" in
        linux)
            for required_tool in ar gcc ranlib; do
                require_tool "$required_tool"
            done
            ;;
        macos)
            for required_tool in ar clang ranlib xcrun; do
                require_tool "$required_tool"
            done
            ;;
    esac
}

verify_binary_inspection_tools() {
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

verify_prepared_tools() {
    verify_common_tools
    verify_native_toolchain

    case "$profile:$family:$platform" in
        source:linux:linux-x64|source:linux:)
            require_tool clang
            require_tool openssl
            require_one_of 'bounded source tests' timeout gtimeout
            verify_binary_inspection_tools
            ;;
        source:linux:linux-arm64)
            require_one_of 'bounded source tests' timeout gtimeout
            verify_binary_inspection_tools
            ;;
        source:macos:*)
            require_one_of 'bounded source tests' timeout gtimeout
            verify_binary_inspection_tools
            ;;
        coverage:linux:*)
            require_tool gcov
            require_tool gcovr
            require_one_of 'bounded coverage tests' timeout gtimeout
            ;;
        coverage:macos:*)
            require_tool gcovr
            require_one_of 'bounded coverage tests' timeout gtimeout
            require_llvm_tool llvm-cov
            require_llvm_tool llvm-profdata
            ;;
        sanitizers:*:*)
            require_tool clang
            require_one_of 'bounded sanitizer tests' timeout gtimeout
            ;;
        debug:linux:*)
            verify_binary_inspection_tools
            require_tool objcopy
            ;;
        debug:macos:*)
            verify_binary_inspection_tools
            require_tool dsymutil
            require_tool dwarfdump
            ;;
        release:linux:*)
            verify_binary_inspection_tools
            require_tool objcopy
            require_tool strip
            ;;
        release:macos:*)
            verify_binary_inspection_tools
            require_tool dsymutil
            require_tool strip
            ;;
    esac
}

report_prepared_packages() {
    case "$family" in
        linux)
            if command -v dpkg-query >/dev/null 2>&1; then
                # Word splitting is intentional: packages is a controlled list.
                # shellcheck disable=SC2086
                dpkg-query -W -f='${Package}=${Version}\n' $packages 2>/dev/null || true
            fi
            ;;
        macos)
            # Word splitting is intentional: packages is a controlled list.
            # shellcheck disable=SC2086
            brew list --versions $packages 2>/dev/null || true
            ;;
    esac
}

case "$profile" in
    dependencies | source | coverage | sanitizers | debug | release)
        ;;
    *)
        fail "unsupported profile: $profile"
        ;;
esac

case "$family" in
    linux)
        packages='build-essential ca-certificates curl file git make perl pkg-config tar xz-utils'
        case "$profile" in
            dependencies)
                ;;
            source)
                # The primary Linux build uses GCC. Clang is installed only for
                # the x64 secondary compiler pass; OpenSSL is needed only by
                # the x64 static-release portability fixture.
                case "$platform" in
                    linux-x64|'')
                        packages="$packages clang openssl"
                        ;;
                    linux-arm64)
                        ;;
                    *)
                        fail "unsupported Linux source platform: $platform"
                        ;;
                esac
                ;;
            coverage)
                packages="$packages gcovr"
                ;;
            sanitizers)
                # Keep one sanitizer implementation across POSIX and Windows.
                packages="$packages clang llvm"
                ;;
            debug)
                ;;
            release)
                packages="$packages zlib1g-dev"
                ;;
        esac
        sudo apt-get update
        # Word splitting is intentional: packages is a controlled internal list.
        # shellcheck disable=SC2086
        sudo apt-get install -y --no-install-recommends $packages
        ;;
    macos)
        # GitHub-hosted macOS images may retain this unused third-party tap.
        # Remove it before Homebrew commands so trust diagnostics stay relevant.
        brew untap aws/tap >/dev/null 2>&1 || true
        case "$profile" in
            dependencies)
                packages='perl pkg-config xz'
                ;;
            source | coverage | sanitizers)
                # GNU timeout is not provided by macOS; coreutils supplies gtimeout.
                packages='coreutils perl pkg-config xz'
                ;;
            debug)
                packages='perl pkg-config xz'
                ;;
            release)
                brew update
                packages='curl perl pkg-config xz'
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

report_prepared_packages
verify_prepared_tools
