
# Purpose: Resolves the native source-test platform and validates the explicit
# dependency prefix. Test runners never bootstrap dependencies implicitly.

# Map the native kernel and architecture to CUP's closed platform identifiers.
cup_test_detect_platform() {
    _cup_test_os=$(uname -s) || return 1
    _cup_test_arch=$(uname -m) || return 1

    case "$_cup_test_os" in
        Linux)
            _cup_test_os=linux
            ;;
        Darwin)
            _cup_test_os=macos
            ;;
        MSYS*|MINGW*|CYGWIN*)
            _cup_test_os=windows
            ;;
        *)
            printf 'Unsupported source-test operating system: %s\n' "$_cup_test_os" >&2
            return 1
            ;;
    esac

    case "$_cup_test_arch" in
        x86_64|amd64)
            _cup_test_arch=x64
            ;;
        arm64|aarch64)
            _cup_test_arch=arm64
            ;;
        *)
            printf 'Unsupported source-test architecture: %s\n' "$_cup_test_arch" >&2
            return 1
            ;;
    esac

    if [ "$_cup_test_os" = windows ] && [ "$_cup_test_arch" != x64 ]; then
        printf 'Unsupported Windows source-test architecture: %s\n' "$_cup_test_arch" >&2
        return 1
    fi
    printf '%s-%s\n' "$_cup_test_os" "$_cup_test_arch"
}

# Resolve the explicit pinned dependency prefix used by all test runners.
cup_test_prepare_environment() {
    _cup_test_platform=${CUP_TEST_PLATFORM:-}

    if [ -z "$_cup_test_platform" ]; then
        _cup_test_platform=$(cup_test_detect_platform) || return 1
    fi
    case "$_cup_test_platform" in
        linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
        *)
            printf 'Unsupported CUP_TEST_PLATFORM: %s\n' "$_cup_test_platform" >&2
            return 1
            ;;
    esac

    CUP_TEST_PLATFORM=$_cup_test_platform
    DEPS_PREFIX=${DEPS_PREFIX:-$HOME/deps/$_cup_test_platform/install}
    export CUP_TEST_PLATFORM DEPS_PREFIX
}

cup_test_find_static_library() {
    _cup_test_name=$1
    for _cup_test_directory in "$DEPS_PREFIX/lib" "$DEPS_PREFIX/lib64"; do
        [ -f "$_cup_test_directory/lib$_cup_test_name.a" ] && {
            printf '%s\n' "$_cup_test_directory/lib$_cup_test_name.a"
            return 0
        }
        [ -f "$_cup_test_directory/lib$_cup_test_name.dll.a" ] && {
            printf '%s\n' "$_cup_test_directory/lib$_cup_test_name.dll.a"
            return 0
        }
    done
    return 1
}

cup_test_dependencies_ready() {
    [ -f "$DEPS_PREFIX/include/argtable3.h" ] &&
        [ -f "$DEPS_PREFIX/include/uthash.h" ] &&
        [ -f "$DEPS_PREFIX/include/unity.h" ] &&
        [ -f "$DEPS_PREFIX/include/unity_internals.h" ] &&
        [ -f "$DEPS_PREFIX/include/event2/event.h" ] &&
        [ -f "$DEPS_PREFIX/include/event2/http.h" ] &&
        [ -f "$DEPS_PREFIX/include/event2/bufferevent.h" ] &&
        [ -f "$DEPS_PREFIX/include/event2/listener.h" ] &&
        [ -f "$DEPS_PREFIX/include/curl/curl.h" ] &&
        [ -f "$DEPS_PREFIX/include/archive.h" ] &&
        [ -f "$DEPS_PREFIX/include/archive_entry.h" ] &&
        [ -f "$DEPS_PREFIX/include/zlib.h" ] &&
        [ -f "$DEPS_PREFIX/include/lzma.h" ] &&
        [ -x "$DEPS_PREFIX/bin/curl-config" ] &&
        { [ -f "$DEPS_PREFIX/lib/pkgconfig/libarchive.pc" ] ||
          [ -f "$DEPS_PREFIX/lib64/pkgconfig/libarchive.pc" ]; } &&
        { [ -f "$DEPS_PREFIX/lib/pkgconfig/libevent_core.pc" ] ||
          [ -f "$DEPS_PREFIX/lib64/pkgconfig/libevent_core.pc" ]; } &&
        { [ -f "$DEPS_PREFIX/lib/pkgconfig/libevent_extra.pc" ] ||
          [ -f "$DEPS_PREFIX/lib64/pkgconfig/libevent_extra.pc" ]; } &&
        cup_test_find_static_library argtable3 >/dev/null &&
        cup_test_find_static_library unity >/dev/null &&
        cup_test_find_static_library event_core >/dev/null &&
        cup_test_find_static_library event_extra >/dev/null &&
        cup_test_find_static_library curl >/dev/null &&
        cup_test_find_static_library archive >/dev/null &&
        cup_test_find_static_library z >/dev/null &&
        cup_test_find_static_library lzma >/dev/null || return 1

    case "$CUP_TEST_PLATFORM" in
        windows-x64)
            return 0
            ;;
    esac
    [ -f "$DEPS_PREFIX/include/openssl/ssl.h" ] &&
        cup_test_find_static_library ssl >/dev/null &&
        cup_test_find_static_library crypto >/dev/null
}

cup_test_require_dependencies() {
    cup_test_dependencies_ready && return 0

    printf '%s\n' \
        "Test dependencies are incomplete in $DEPS_PREFIX." \
        "Run 'make PLATFORM=$CUP_TEST_PLATFORM deps' before the tests," \
        'or set DEPS_PREFIX to an explicitly prepared native prefix.' >&2
    return 1
}

# Print actionable installation guidance for optional quality tools.
cup_test_tool_hint() {
    _cup_test_tool=$1
    case "$CUP_TEST_PLATFORM" in
        linux-*)
            case "$_cup_test_tool" in
                gcovr)
                    printf '%s\n' "Install it with: sudo apt-get install gcovr" >&2
                    ;;
                gcc|gcov)
                    printf '%s\n' "Install GCC coverage tools with: sudo apt-get install build-essential" >&2
                    ;;
                clang|llvm-cov|llvm-profdata|llvm-symbolizer)
                    printf '%s\n' \
                        "Install LLVM tools with: sudo apt-get install clang llvm" >&2
                    ;;
                timeout)
                    printf '%s\n' "Install it with: sudo apt-get install coreutils" >&2
                    ;;
                *)
                    printf '%s\n' "Install '$_cup_test_tool' with your system package manager." >&2
                    ;;
            esac
            ;;
        macos-*)
            case "$_cup_test_tool" in
                gcovr)
                    printf '%s\n' "Install it with: brew install gcovr" >&2
                    ;;
                timeout|gtimeout)
                    printf '%s\n' "Install GNU timeout with: brew install coreutils" >&2
                    ;;
                clang|llvm-cov|llvm-profdata|llvm-symbolizer)
                    printf '%s\n' \
                        "Install Xcode Command Line Tools with: xcode-select --install" >&2
                    ;;
                *)
                    printf '%s\n' "Install '$_cup_test_tool' with Homebrew or Xcode Command Line Tools." >&2
                    ;;
            esac
            ;;
        windows-x64)
            case "$_cup_test_tool" in
                gcovr)
                    printf '%s\n' "Install it in UCRT64 with: pacman -S mingw-w64-ucrt-x86_64-gcovr" >&2
                    ;;
                gcc|gcov)
                    printf '%s\n' "Install GCC tools in UCRT64 with: pacman -S mingw-w64-ucrt-x86_64-gcc" >&2
                    ;;
                clang|llvm-cov|llvm-profdata|llvm-symbolizer)
                    if [ "${MSYSTEM:-}" = CLANG64 ]; then
                        _cup_test_package_prefix=mingw-w64-clang-x86_64
                        _cup_test_environment=CLANG64
                    else
                        _cup_test_package_prefix=mingw-w64-ucrt-x86_64
                        _cup_test_environment=UCRT64
                    fi
                    printf '%s\n' \
                        "Install LLVM tools in $_cup_test_environment with:" \
                        "  pacman -S ${_cup_test_package_prefix}-clang" \
                        "    ${_cup_test_package_prefix}-compiler-rt" \
                        "    ${_cup_test_package_prefix}-llvm-tools" >&2
                    ;;
                timeout)
                    printf '%s\n' "Install it in MSYS2 with: pacman -S coreutils" >&2
                    ;;
                powershell.exe)
                    printf '%s\n' "PowerShell is required from the Windows host." >&2
                    ;;
                *)
                    printf '%s\n' \
                        "Install '$_cup_test_tool' in the active MSYS2" \
                        "${MSYSTEM:-UCRT64} environment." >&2
                    ;;
            esac
            ;;
    esac
}

cup_test_require_tool() {
    _cup_test_tool=$1
    _cup_test_purpose=${2:-the requested test target}
    command -v "$_cup_test_tool" >/dev/null 2>&1 && return 0
    printf "Required tool '%s' was not found; it is needed for %s.\n" \
        "$_cup_test_tool" "$_cup_test_purpose" >&2
    cup_test_tool_hint "$_cup_test_tool"
    return 1
}

cup_test_find_timeout() {
    if [ -n "${CUP_TEST_TIMEOUT_COMMAND:-}" ]; then
        command -v "$CUP_TEST_TIMEOUT_COMMAND" >/dev/null 2>&1 || {
            printf "Configured timeout command '%s' was not found.\n" \
                "$CUP_TEST_TIMEOUT_COMMAND" >&2
            return 1
        }
        printf '%s\n' "$CUP_TEST_TIMEOUT_COMMAND"
        return 0
    fi
    for _cup_test_timeout in timeout gtimeout; do
        if command -v "$_cup_test_timeout" >/dev/null 2>&1; then
            printf '%s\n' "$_cup_test_timeout"
            return 0
        fi
    done
    printf '%s\n' "A timeout command is required for bounded quality tests." >&2
    cup_test_tool_hint timeout
    return 1
}

cup_test_find_llvm_tool() {
    _cup_test_tool=$1
    if command -v "$_cup_test_tool" >/dev/null 2>&1; then
        command -v "$_cup_test_tool"
        return 0
    fi
    if command -v xcrun >/dev/null 2>&1; then
        xcrun --find "$_cup_test_tool" 2>/dev/null && return 0
    fi
    printf "Required LLVM tool '%s' was not found.\n" "$_cup_test_tool" >&2
    cup_test_tool_hint "$_cup_test_tool"
    return 1
}

# Source-based LLVM coverage support was added by gcovr 8.5.
cup_test_require_gcovr_llvm() {
    _cup_test_version=$(gcovr --version 2>/dev/null | awk '
        NR == 1 {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^[0-9]+\.[0-9]+([.][0-9]+)?/) {
                    print $i
                    exit
                }
            }
        }
    ')
    _cup_test_major=${_cup_test_version%%.*}
    _cup_test_remainder=${_cup_test_version#*.}
    _cup_test_minor=${_cup_test_remainder%%.*}
    case "$_cup_test_major:$_cup_test_minor" in
        *[!0-9:]*|:*)
            printf 'Unable to determine the installed gcovr version.\n' >&2
            return 1
            ;;
    esac
    if [ "$_cup_test_major" -lt 8 ] || {
        [ "$_cup_test_major" -eq 8 ] && [ "$_cup_test_minor" -lt 5 ]
    }; then
        printf 'gcovr 8.5 or newer is required for LLVM source-based coverage; found %s.\n' \
            "$_cup_test_version" >&2
        cup_test_tool_hint gcovr
        return 1
    fi
}
