#!/bin/sh

# Purpose: Resolves the native source-test platform and validates the explicit
# dependency prefix. Test runners never bootstrap dependencies implicitly.

cup_test_detect_platform() {
    _cup_test_os=$(uname -s) || return 1
    _cup_test_arch=$(uname -m) || return 1

    case "$_cup_test_os" in
        Linux) _cup_test_os=linux ;;
        Darwin) _cup_test_os=macos ;;
        MSYS*|MINGW*|CYGWIN*) _cup_test_os=windows ;;
        *)
            printf 'Unsupported source-test operating system: %s\n' "$_cup_test_os" >&2
            return 1
            ;;
    esac

    case "$_cup_test_arch" in
        x86_64|amd64) _cup_test_arch=x64 ;;
        arm64|aarch64) _cup_test_arch=arm64 ;;
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
        [ -f "$DEPS_PREFIX/include/curl/curl.h" ] &&
        [ -f "$DEPS_PREFIX/include/archive.h" ] &&
        [ -f "$DEPS_PREFIX/include/archive_entry.h" ] &&
        [ -f "$DEPS_PREFIX/include/zlib.h" ] &&
        [ -f "$DEPS_PREFIX/include/lzma.h" ] &&
        [ -x "$DEPS_PREFIX/bin/curl-config" ] &&
        { [ -f "$DEPS_PREFIX/lib/pkgconfig/libarchive.pc" ] ||
          [ -f "$DEPS_PREFIX/lib64/pkgconfig/libarchive.pc" ]; } &&
        cup_test_find_static_library argtable3 >/dev/null &&
        cup_test_find_static_library unity >/dev/null &&
        cup_test_find_static_library curl >/dev/null &&
        cup_test_find_static_library archive >/dev/null &&
        cup_test_find_static_library z >/dev/null &&
        cup_test_find_static_library lzma >/dev/null || return 1

    case "$CUP_TEST_PLATFORM" in
        windows-x64) return 0 ;;
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
