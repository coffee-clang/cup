#!/usr/bin/env bash
set -euo pipefail

PLATFORM="linux-x64"

ZLIB_VERSION="${ZLIB_VERSION:-1.3.2}"
XZ_VERSION="${XZ_VERSION:-5.8.3}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.6}"
CURL_VERSION="${CURL_VERSION:-8.20.0}"
LIBARCHIVE_VERSION="${LIBARCHIVE_VERSION:-3.8.7}"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
PREFIX="${PREFIX:-$DEPS_ROOT/install}"

JOBS="${JOBS:-$(nproc)}"

ZLIB_URL="https://zlib.net/zlib-${ZLIB_VERSION}.tar.gz"
XZ_URL="https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.xz"
OPENSSL_URL="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.xz"
LIBARCHIVE_URL="https://libarchive.org/downloads/libarchive-${LIBARCHIVE_VERSION}.tar.xz"

download() {
    url="$1"
    output="$2"

    if [ -f "$output" ]; then
        echo "==> Using cached $(basename "$output")"
        return 0
    fi

    echo "==> Downloading $url"
    curl -L "$url" -o "$output"
}

extract() {
    archive="$1"
    destination="$2"

    rm -rf "$destination"
    mkdir -p "$destination"

    case "$archive" in
        *.tar.gz|*.tgz)
            tar -xzf "$archive" -C "$destination" --strip-components=1
            ;;
        *.tar.xz)
            tar -xJf "$archive" -C "$destination" --strip-components=1
            ;;
        *)
            echo "Error: unsupported archive format '$archive'." >&2
            exit 1
            ;;
    esac
}

build_zlib() {
    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download "$ZLIB_URL" "$archive"
    extract "$archive" "$source"

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

    download "$XZ_URL" "$archive"
    extract "$archive" "$source"

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

    download "$OPENSSL_URL" "$archive"
    extract "$archive" "$source"

    echo "==> Building OpenSSL ${OPENSSL_VERSION}"
    cd "$source"

    ./Configure linux-x86_64 \
        no-shared \
        no-tests \
        --prefix="$PREFIX" \
        --openssldir="/etc/ssl"

    make -j"$JOBS"
    make install_sw
}

build_curl() {
    archive="$SRC_DIR/curl-${CURL_VERSION}.tar.xz"
    source="$BUILD_DIR/curl-${CURL_VERSION}"

    download "$CURL_URL" "$archive"
    extract "$archive" "$source"

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

    download "$LIBARCHIVE_URL" "$archive"
    extract "$archive" "$source"

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