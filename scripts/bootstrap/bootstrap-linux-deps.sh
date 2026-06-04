#!/usr/bin/env bash
set -euo pipefail

PLATFORM="${PLATFORM:-linux-x64}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source "$SCRIPT_DIR/bootstrap-common.sh"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
PREFIX="${PREFIX:-$DEPS_ROOT/install}"

JOBS="${JOBS:-$(nproc)}"

case "$PLATFORM" in
    linux-x64)
        OPENSSL_TARGET="${OPENSSL_TARGET:-linux-x86_64}"
        ;;
    linux-arm64)
        OPENSSL_TARGET="${OPENSSL_TARGET:-linux-aarch64}"
        ;;
    *)
        echo "Error: unsupported Linux dependency platform '$PLATFORM'." >&2
        exit 1
        ;;
esac

build_zlib() {
    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download_source zlib "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building zlib ${ZLIB_VERSION}"
    cd "$source"

    CHOST="" ./configure \
        --prefix="$PREFIX" \
        --static

    make -j"$JOBS"
    make install
}

build_xz() {
    archive="$SRC_DIR/xz-${XZ_VERSION}.tar.xz"
    source="$BUILD_DIR/xz-${XZ_VERSION}"

    download_source xz "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building xz ${XZ_VERSION}"
    cd "$source"

    ./configure \
        --prefix="$PREFIX" \
        --disable-shared \
        --enable-static

    make -j"$JOBS"
    make install
}

build_openssl() {
    archive="$SRC_DIR/openssl-${OPENSSL_VERSION}.tar.gz"
    source="$BUILD_DIR/openssl-${OPENSSL_VERSION}"

    download_source openssl "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building OpenSSL ${OPENSSL_VERSION}"
    cd "$source"

    ./Configure "$OPENSSL_TARGET" \
        no-shared \
        no-tests \
        no-autoload-config \
        --prefix="$PREFIX" \
        --openssldir="$PREFIX/ssl"

    make -j"$JOBS"
    make install_sw
}

build_curl() {
    archive="$SRC_DIR/curl-${CURL_VERSION}.tar.xz"
    source="$BUILD_DIR/curl-${CURL_VERSION}"

    download_source curl "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building curl ${CURL_VERSION}"
    cd "$source"

    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    ./configure \
        --prefix="$PREFIX" \
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
        --disable-manual

    make -j"$JOBS"
    make install
}

build_libarchive() {
    archive="$SRC_DIR/libarchive-${LIBARCHIVE_VERSION}.tar.xz"
    source="$BUILD_DIR/libarchive-${LIBARCHIVE_VERSION}"

    download_source libarchive "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building libarchive ${LIBARCHIVE_VERSION}"
    cd "$source"

    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    ./configure \
        --prefix="$PREFIX" \
        --disable-shared \
        --enable-static \
        --without-bz2lib \
        --without-lzo2 \
        --without-lz4 \
        --without-zstd \
        --without-xml2 \
        --without-expat \
        --without-nettle \
        --without-openssl

    make -j"$JOBS"
    make install
}

verify() {
    echo "==> Verifying generated link metadata"

    if [ ! -x "$PREFIX/bin/curl-config" ]; then
        echo "Error: curl-config was not installed." >&2
        exit 1
    fi

    "$PREFIX/bin/curl-config" --static-libs

    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
        pkg-config --static --libs libarchive

    echo "==> Linux dependencies installed in $PREFIX"
}

main() {
    mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX"

    build_zlib
    build_xz
    build_openssl
    build_curl
    build_libarchive
    verify
}

main "$@"
