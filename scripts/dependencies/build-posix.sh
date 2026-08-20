#!/usr/bin/env bash

# Builds one complete transactional dependency prefix on native Linux
# or macOS. Platform-specific settings are selected here instead of through
# thin wrapper scripts.
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
PLATFORM="${PLATFORM:-}"
REQUESTED_MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-}"

if [ -z "$PLATFORM" ]; then
    case "$(uname -s):$(uname -m)" in
        Linux:x86_64|Linux:amd64)
            PLATFORM=linux-x64
            ;;
        Linux:aarch64|Linux:arm64)
            PLATFORM=linux-arm64
            ;;
        Darwin:x86_64|Darwin:amd64)
            PLATFORM=macos-x64
            ;;
        Darwin:arm64|Darwin:aarch64)
            PLATFORM=macos-arm64
            ;;
        *)
            echo "Error: unable to select a supported native dependency platform." >&2
            exit 1
            ;;
    esac
fi

case "$PLATFORM" in
    linux-x64)
        CC=gcc
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=linux-x86_64
        CUP_POSIX_BOOTSTRAP_LABEL=Linux
        CUP_POSIX_BOOTSTRAP_LIB64=1
        ;;
    linux-arm64)
        CC=gcc
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=linux-aarch64
        CUP_POSIX_BOOTSTRAP_LABEL=Linux
        CUP_POSIX_BOOTSTRAP_LIB64=1
        ;;
    macos-x64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-x86_64-cc
        CUP_POSIX_BOOTSTRAP_LABEL=macOS
        CUP_POSIX_BOOTSTRAP_LIB64=0
        ;;
    macos-arm64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-arm64-cc
        CUP_POSIX_BOOTSTRAP_LABEL=macOS
        CUP_POSIX_BOOTSTRAP_LIB64=0
        ;;
    *)
        echo "Error: unsupported POSIX dependency platform '$PLATFORM'." >&2
        exit 1
        ;;
esac

detect_native_platform() {
    case "$(uname -s):$(uname -m)" in
        Linux:x86_64|Linux:amd64) printf '%s\n' linux-x64 ;;
        Linux:aarch64|Linux:arm64) printf '%s\n' linux-arm64 ;;
        Darwin:x86_64|Darwin:amd64) printf '%s\n' macos-x64 ;;
        Darwin:arm64|Darwin:aarch64) printf '%s\n' macos-arm64 ;;
        *) return 1 ;;
    esac
}

native_platform=$(detect_native_platform) || {
    echo "Error: unable to identify a supported native dependency platform." >&2
    exit 1
}
[ "$PLATFORM" = "$native_platform" ] || {
    echo "Error: dependency platform '$PLATFORM' does not match native host '$native_platform'." >&2
    exit 1
}

case "$CUP_POSIX_BOOTSTRAP_LIB64" in
    0 | 1)
        ;;
    *)
        exit 1
        ;;
esac

case "$PLATFORM" in
    macos-*)
        DEPENDENCY_PROFILE=$(dependency_profile "$PLATFORM")
        CANONICAL_MACOSX_DEPLOYMENT_TARGET=$(
            dependency_macos_deployment_target "$PLATFORM" "$DEPENDENCY_PROFILE"
        )
        if [ -n "$REQUESTED_MACOSX_DEPLOYMENT_TARGET" ] &&
            [ "$REQUESTED_MACOSX_DEPLOYMENT_TARGET" != "$CANONICAL_MACOSX_DEPLOYMENT_TARGET" ]; then
            echo "Error: macOS dependencies require MACOSX_DEPLOYMENT_TARGET=$CANONICAL_MACOSX_DEPLOYMENT_TARGET." >&2
            exit 1
        fi
        MACOSX_DEPLOYMENT_TARGET=$CANONICAL_MACOSX_DEPLOYMENT_TARGET
        export MACOSX_DEPLOYMENT_TARGET
        ;;
esac

dependency_normalize_build_environment
JOBS="$(dependency_resolve_jobs)"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
DEPS_PREFIX="${DEPS_PREFIX:-$DEPS_ROOT/install}"
PREFIX="$DEPS_PREFIX"

dependency_require_whitespace_free_path "dependency root" "$DEPS_ROOT"
dependency_require_whitespace_free_path "dependency source directory" "$SRC_DIR"
dependency_require_whitespace_free_path "dependency build directory" "$BUILD_DIR"
dependency_require_whitespace_free_path "dependency prefix" "$DEPS_PREFIX"

library_flags() {
    if [ "$CUP_POSIX_BOOTSTRAP_LIB64" = 1 ]; then
        printf '%s' "-L$PREFIX/lib -L$PREFIX/lib64"
    else
        printf '%s' "-L$PREFIX/lib"
    fi
}

pkg_config_dirs() {
    if [ "$CUP_POSIX_BOOTSTRAP_LIB64" = 1 ]; then
        printf '%s' "$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig"
    else
        printf '%s' "$PREFIX/lib/pkgconfig"
    fi
}

# Static third-party dependency builders.
build_zlib() {
    local archive
    local source

    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download_source zlib "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building zlib ${ZLIB_VERSION}"
    cd "$source"

    CC="$CC" CFLAGS="$CUP_DEPENDENCY_CFLAGS" CHOST="" ./configure \
        --prefix="$INSTALL_PREFIX" \
        --static

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

build_xz() {
    local archive
    local source

    archive="$SRC_DIR/xz-${XZ_VERSION}.tar.xz"
    source="$BUILD_DIR/xz-${XZ_VERSION}"

    download_source xz "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building xz ${XZ_VERSION}"
    cd "$source"

    CC="$CC" CFLAGS="$CUP_DEPENDENCY_CFLAGS" ./configure \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-nls \
        --disable-xz \
        --disable-xzdec \
        --disable-lzmadec \
        --disable-lzmainfo \
        --disable-scripts \
        --disable-doc

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

build_openssl() {
    local archive
    local source
    local neutral_prefix=/__cup_runtime__/openssl
    local install_root
    local payload
    local openssl_cflags

    archive="$SRC_DIR/openssl-${OPENSSL_VERSION}.tar.gz"
    source="$BUILD_DIR/openssl-${OPENSSL_VERSION}"
    install_root="$BUILD_DIR/openssl-${OPENSSL_VERSION}-install"
    payload="$install_root$neutral_prefix"

    download_source openssl "$archive"
    extract_archive "$archive" "$source"
    if [ -e "$install_root" ] || [ -L "$install_root" ]; then
        cup_path_remove_child_tree "$BUILD_DIR" "$install_root" \
            'OpenSSL install root' || return 1
    fi

    echo "==> Building OpenSSL ${OPENSSL_VERSION} for ${OPENSSL_TARGET}"
    cd "$source"
    # OpenSSL records both its configured directories and CFLAGS in libcrypto.
    # Keep runtime directories neutral and remove path-bearing prefix-map flags
    # from the exposed build information. OpenSSL compiles from relative source
    # paths without debug information, so the remaining flags are reproducible.
    openssl_cflags=$(dependency_buildinfo_safe_cflags "$CUP_DEPENDENCY_CFLAGS")
    CC="$CC" AR="$AR" RANLIB="$RANLIB" \
        CFLAGS="$openssl_cflags" \
        ./Configure "$OPENSSL_TARGET" \
            --prefix="$neutral_prefix" \
            --openssldir="$neutral_prefix" \
            no-shared no-tests no-apps no-docs no-autoload-config no-dso
    make -j"$JOBS" build_libs
    make install_dev DESTDIR="$install_root"
    [ -d "$payload" ] || {
        echo "Error: OpenSSL neutral installation payload was not produced." >&2
        return 1
    }
    cup_path_copy_tree "$payload" "$PREFIX"
    normalize_dependency_metadata "$PREFIX" "$neutral_prefix" "$INSTALL_PREFIX"
}

build_curl() {
    local archive
    local source
    local pkg_dirs

    archive="$SRC_DIR/curl-${CURL_VERSION}.tar.xz"
    source="$BUILD_DIR/curl-${CURL_VERSION}"
    pkg_dirs="$(pkg_config_dirs)"

    download_source curl "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building curl ${CURL_VERSION}"
    cd "$source"

    CC="$CC" CFLAGS="$CUP_DEPENDENCY_CFLAGS" \
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="$(library_flags)" \
    PKG_CONFIG_PATH="$pkg_dirs" \
    PKG_CONFIG_LIBDIR="$pkg_dirs" \
    PKG_CONFIG_SYSROOT_DIR="" \
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --with-openssl="$PREFIX" \
        --enable-ares="$PREFIX" \
        --with-zlib="$PREFIX" \
        --without-ca-bundle \
        --without-ca-path \
        --without-brotli \
        --without-zstd \
        --without-nghttp2 \
        --without-nghttp3 \
        --without-ngtcp2 \
        --without-libidn2 \
        --without-libpsl \
        --disable-ldap \
        --disable-ldaps \
        --disable-rtsp \
        --disable-dict \
        --disable-telnet \
        --disable-tftp \
        --disable-pop3 \
        --disable-imap \
        --disable-smtp \
        --disable-gopher \
        --disable-mqtt \
        --disable-netrc \
        --disable-manual

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

build_libarchive() {
    local archive
    local source
    local pkg_dirs

    archive="$SRC_DIR/libarchive-${LIBARCHIVE_VERSION}.tar.xz"
    source="$BUILD_DIR/libarchive-${LIBARCHIVE_VERSION}"
    pkg_dirs="$(pkg_config_dirs)"

    download_source libarchive "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building libarchive ${LIBARCHIVE_VERSION}"
    cd "$source"

    CC="$CC" CFLAGS="$CUP_DEPENDENCY_CFLAGS" \
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="$(library_flags)" \
    PKG_CONFIG_PATH="$pkg_dirs" \
    PKG_CONFIG_LIBDIR="$pkg_dirs" \
    PKG_CONFIG_SYSROOT_DIR="" \
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-acl \
        --without-bz2lib \
        --without-lzo2 \
        --without-lz4 \
        --without-zstd \
        --without-xml2 \
        --without-expat \
        --without-nettle \
        --without-openssl

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

# Final prefix and static metadata verification.
verify() {
    echo "==> Verifying generated dependency prefix"
    dependency_prefix_complete "$PREFIX" 1 "$CUP_DEPS_FINAL_PREFIX" || {
        echo "Error: generated dependency prefix is incomplete or unsafe." >&2
        exit 1
    }
    echo "==> $CUP_POSIX_BOOTSTRAP_LABEL dependencies verified for $CUP_DEPS_FINAL_PREFIX"
}

main() {
    local profile
    local metadata
    local compiler_target

    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_tool cmp
    require_sha256_tool
    compiler_target=$("$CC" -dumpmachine 2>/dev/null || "$CC" -print-target-triple 2>/dev/null)
    case "$PLATFORM:$compiler_target" in
        linux-x64:*x86_64*|linux-arm64:*aarch64*|linux-arm64:*arm64*|\
        macos-x64:*x86_64*apple*|macos-arm64:*arm64*apple*|macos-arm64:*aarch64*apple*) ;;
        *)
            echo "Error: compiler target '$compiler_target' does not match $PLATFORM." >&2
            exit 1
            ;;
    esac
    profile=$(dependency_profile "$PLATFORM")
    metadata=$(dependency_metadata "$PLATFORM" "$profile")
    dependency_acquire_build_lock "$DEPS_ROOT"
    trap 'abort_dependency_prefix; dependency_release_build_lock' EXIT
    prepare_dependency_prefix "$DEPS_PREFIX" "$metadata" 1
    if [ "$CUP_DEPS_PREFIX_READY" = 1 ]; then
        dependency_release_build_lock
        trap - EXIT
        exit 0
    fi
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    require_tool curl
    require_tool tar
    require_tool make
    require_tool mktemp
    require_tool perl
    require_tool pkg-config
    DESTDIR="$CUP_DEPS_STAGE_ROOT"
    INSTALL_PREFIX="$CUP_DEPS_FINAL_PREFIX"
    PREFIX="$CUP_DEPS_BUILD_PREFIX"
    CUP_DEPENDENCY_CFLAGS=$(dependency_reproducible_cflags "$CC" \
        "$DEPS_ROOT" "$BUILD_DIR" "$CUP_DEPS_STAGE_ROOT")
    export CUP_DEPENDENCY_CFLAGS

    cup_path_prepare_child_directory "$DEPS_ROOT" "$SRC_DIR" \
        "dependency source directory"
    cup_path_prepare_child_directory "$DEPS_ROOT" "$BUILD_DIR" \
        "dependency build directory"
    cup_path_check_directory_chain "$PREFIX" 0 "dependency build prefix"

    build_zlib
    build_xz
    build_openssl
    build_cares_static "$SRC_DIR" "$BUILD_DIR" "$CC" "$AR" "$RANLIB"
    build_curl
    build_libarchive
    build_libevent_static "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    normalize_dependency_metadata "$PREFIX" \
        "$CUP_DEPS_BUILD_PREFIX" "$CUP_DEPS_FINAL_PREFIX"
    verify
    finish_dependency_prefix "$PREFIX"
    dependency_release_build_lock
    trap - EXIT HUP INT TERM
}

main "$@"
