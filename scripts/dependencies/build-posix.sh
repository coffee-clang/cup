#!/usr/bin/env bash

# Purpose: Builds one complete transactional dependency prefix on native Linux
# or macOS. Platform-specific settings are selected here instead of through
# thin wrapper scripts.
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
PLATFORM="${PLATFORM:-}"
REQUESTED_MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-}"

if [ -z "$PLATFORM" ]; then
    case "$(uname -s):$(uname -m)" in
        Linux:x86_64|Linux:amd64) PLATFORM=linux-x64 ;;
        Linux:aarch64|Linux:arm64) PLATFORM=linux-arm64 ;;
        Darwin:x86_64|Darwin:amd64) PLATFORM=macos-x64 ;;
        Darwin:arm64|Darwin:aarch64) PLATFORM=macos-arm64 ;;
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
        CUP_POSIX_PLATFORM_POLICY=glibc-native
        ;;
    linux-arm64)
        CC=gcc
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=linux-aarch64
        CUP_POSIX_BOOTSTRAP_LABEL=Linux
        CUP_POSIX_BOOTSTRAP_LIB64=1
        CUP_POSIX_PLATFORM_POLICY=glibc-native
        ;;
    macos-x64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-x86_64-cc
        CUP_POSIX_BOOTSTRAP_LABEL=macOS
        CUP_POSIX_BOOTSTRAP_LIB64=0
        CUP_POSIX_PLATFORM_POLICY=macos-deployment-13.0
        ;;
    macos-arm64)
        CC=clang
        AR=ar
        RANLIB=ranlib
        OPENSSL_TARGET=darwin64-arm64-cc
        CUP_POSIX_BOOTSTRAP_LABEL=macOS
        CUP_POSIX_BOOTSTRAP_LIB64=0
        CUP_POSIX_PLATFORM_POLICY=macos-deployment-13.0
        ;;
    *)
        echo "Error: unsupported POSIX dependency platform '$PLATFORM'." >&2
        exit 1
        ;;
esac

case "$CUP_POSIX_BOOTSTRAP_LIB64" in
    0 | 1)
        ;;
    *)
        exit 1
        ;;
esac

case "$PLATFORM" in
    macos-*)
        if [ -n "$REQUESTED_MACOSX_DEPLOYMENT_TARGET" ] &&
            [ "$REQUESTED_MACOSX_DEPLOYMENT_TARGET" != 13.0 ]; then
            echo "Error: macOS dependencies require MACOSX_DEPLOYMENT_TARGET=13.0." >&2
            exit 1
        fi
        MACOSX_DEPLOYMENT_TARGET=13.0
        export MACOSX_DEPLOYMENT_TARGET
        ;;
esac

# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
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

    CFLAGS="$CUP_DEPENDENCY_CFLAGS" CHOST="" ./configure \
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

    CFLAGS="$CUP_DEPENDENCY_CFLAGS" ./configure \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-nls

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

build_openssl() {
    local archive
    local source
    local neutral_prefix=/__cup_runtime__/openssl
    local install_root
    local payload

    archive="$SRC_DIR/openssl-${OPENSSL_VERSION}.tar.gz"
    source="$BUILD_DIR/openssl-${OPENSSL_VERSION}"
    install_root="$BUILD_DIR/openssl-${OPENSSL_VERSION}-install"
    payload="$install_root$neutral_prefix"

    download_source openssl "$archive"
    extract_archive "$archive" "$source"
    rm -rf "$install_root"

    echo "==> Building OpenSSL ${OPENSSL_VERSION} for ${OPENSSL_TARGET}"
    cd "$source"
    # OpenSSL records --prefix/--openssldir in libcrypto. Use a deterministic,
    # deliberately nonexistent runtime namespace, then relocate only generated
    # build metadata to the actual transactional CUP prefix.
    # shellcheck disable=SC2086
    CC="$CC" AR="$AR" RANLIB="$RANLIB" \
        CFLAGS="$CUP_DEPENDENCY_CFLAGS" \
        ./Configure "$OPENSSL_TARGET" \
            --prefix="$neutral_prefix" \
            --openssldir="$neutral_prefix" \
            no-shared no-tests no-autoload-config no-dso
    make -j"$JOBS"
    make install_sw DESTDIR="$install_root"
    [ -d "$payload" ] || {
        echo "Error: OpenSSL neutral installation payload was not produced." >&2
        return 1
    }
    cp -R "$payload"/. "$PREFIX"/
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

    CFLAGS="$CUP_DEPENDENCY_CFLAGS" \
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

    CFLAGS="$CUP_DEPENDENCY_CFLAGS" \
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="$(library_flags)" \
    PKG_CONFIG_PATH="$pkg_dirs" \
    PKG_CONFIG_LIBDIR="$pkg_dirs" \
    PKG_CONFIG_SYSROOT_DIR="" \
    ./configure \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-nls \
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
    local pkg_dirs
    local curl_flags
    local archive_flags
    local event_flags

    pkg_dirs="$(pkg_config_dirs)"

    echo "==> Verifying generated link metadata"
    if [ ! -x "$PREFIX/bin/curl-config" ]; then
        echo "Error: curl-config was not installed." >&2
        exit 1
    fi

    curl_flags="$("$PREFIX/bin/curl-config" --static-libs)"
    archive_flags="$(PKG_CONFIG_PATH="$pkg_dirs" \
        PKG_CONFIG_LIBDIR="$pkg_dirs" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libarchive)"
    event_flags="$(PKG_CONFIG_PATH="$pkg_dirs" \
        PKG_CONFIG_LIBDIR="$pkg_dirs" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libevent_extra libevent_core)"
    if [ -z "$curl_flags" ] || [ -z "$archive_flags" ] || [ -z "$event_flags" ]; then
        echo "Error: generated static link metadata is empty." >&2
        exit 1
    fi
    if [ -n "$CUP_DEPS_STAGE_ROOT" ]; then
        case "$curl_flags $archive_flags $event_flags" in
            *"$CUP_DEPS_STAGE_ROOT"*)
                echo "Error: generated link metadata contains the staging path." >&2
                exit 1
                ;;
        esac
    fi

    if ! dependency_prefix_complete "$PREFIX" 1 "$CUP_DEPS_FINAL_PREFIX"; then
        echo "Error: generated dependency prefix is incomplete." >&2
        exit 1
    fi

    printf '%s\n' "$curl_flags"
    printf '%s\n' "$archive_flags"
    printf '%s\n' "$event_flags"
    echo "==> $CUP_POSIX_BOOTSTRAP_LABEL dependencies verified for $CUP_DEPS_FINAL_PREFIX"
}

main() {
    local toolchain
    local id
    local metadata

    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_sha256_tool
    toolchain=$(dependency_posix_toolchain_identity \
        "$CC" "$AR" "$RANLIB" "$OPENSSL_TARGET" \
        "$CUP_POSIX_PLATFORM_POLICY")
    id=$(dependency_id "$PLATFORM" "$toolchain" 1 "$PROJECT_ROOT" \
        "$SCRIPT_DIR/sources.sh" \
        "$SCRIPT_DIR/common.sh" \
        "$SCRIPT_DIR/build-posix.sh")
    metadata=$(dependency_metadata "$PLATFORM" "$id")
    prepare_dependency_prefix "$DEPS_PREFIX" "$metadata" 1
    if [ "$CUP_DEPS_PREFIX_READY" = 1 ]; then
        exit 0
    fi
    trap 'abort_dependency_prefix' EXIT HUP INT TERM
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

    mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX"

    build_zlib
    build_xz
    build_openssl
    build_curl
    build_libarchive
    build_libevent_static "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    normalize_dependency_metadata "$PREFIX" \
        "$CUP_DEPS_BUILD_PREFIX" "$CUP_DEPS_FINAL_PREFIX"
    verify
    finish_dependency_prefix "$PREFIX"
    trap - EXIT HUP INT TERM
}

main "$@"
