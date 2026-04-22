#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Static dependency bootstrap for CUP
# Builds:
#   - zlib
#   - xz / liblzma
#   - OpenSSL
#   - curl
#   - libarchive
#
# Default install prefix:
#   $HOME/deps/install
#
# You can override:
#   PREFIX=/custom/prefix bash bootstrap-static-deps.sh
#   OPENSSL_TARGET=linux-x86_64 bash bootstrap-static-deps.sh
# ============================================================

# Versions
ZLIB_VERSION="1.3.2"
XZ_VERSION="5.8.1"
OPENSSL_VERSION="3.5.1"
CURL_VERSION="8.16.0"
LIBARCHIVE_VERSION="3.8.1"

# Paths
PREFIX="${PREFIX:-$HOME/deps/install}"
SRC_DIR="${SRC_DIR:-$HOME/deps/src}"
BUILD_DIR="${BUILD_DIR:-$HOME/deps/build}"
DOWNLOAD_DIR="${DOWNLOAD_DIR:-$HOME/deps/downloads}"

# OpenSSL target for Linux x86_64 by default
OPENSSL_TARGET="${OPENSSL_TARGET:-linux-x86_64}"

# URLs
ZLIB_URL="https://zlib.net/zlib-${ZLIB_VERSION}.tar.gz"
XZ_URL="https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.gz"
LIBARCHIVE_URL="https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VERSION}/libarchive-${LIBARCHIVE_VERSION}.tar.gz"

# Helpers
log() {
    printf '\n==> %s\n' "$1"
}

die() {
    fprintf() { :; }
    echo "Error: $1" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command '$1' not found"
}

download() {
    local url="$1"
    local out="$2"

    if [ -f "$out" ]; then
        log "Already downloaded: $(basename "$out")"
        return 0
    fi

    log "Downloading $(basename "$out")"
    curl -fL "$url" -o "$out"
}

extract_tarball() {
    local tarball="$1"
    local out_dir="$2"

    mkdir -p "$out_dir"

    log "Extracting $(basename "$tarball")"
    tar -xf "$tarball" -C "$out_dir"
}


# Checks
require_cmd curl
require_cmd tar
require_cmd make
require_cmd gcc
require_cmd cmake
require_cmd perl
require_cmd pkg-config

mkdir -p "$PREFIX" "$SRC_DIR" "$BUILD_DIR" "$DOWNLOAD_DIR"

# Export search paths so configure/cmake can find previous deps
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# 1. zlib
ZLIB_TARBALL="$DOWNLOAD_DIR/zlib-${ZLIB_VERSION}.tar.gz"
ZLIB_SRC="$SRC_DIR/zlib-${ZLIB_VERSION}"

download "$ZLIB_URL" "$ZLIB_TARBALL"
if [ ! -d "$ZLIB_SRC" ]; then
    extract_tarball "$ZLIB_TARBALL" "$SRC_DIR"
fi

log "Building zlib ${ZLIB_VERSION}"
(
    cd "$ZLIB_SRC"
    ./configure --static --prefix="$PREFIX"
    make -j"$(nproc)"
    make install
)

# 2. xz / liblzma
XZ_TARBALL="$DOWNLOAD_DIR/xz-${XZ_VERSION}.tar.gz"
XZ_SRC="$SRC_DIR/xz-${XZ_VERSION}"

download "$XZ_URL" "$XZ_TARBALL"
if [ ! -d "$XZ_SRC" ]; then
    extract_tarball "$XZ_TARBALL" "$SRC_DIR"
fi

log "Building xz/liblzma ${XZ_VERSION}"
(
    cd "$XZ_SRC"
    ./configure --disable-shared --enable-static --prefix="$PREFIX"
    make -j"$(nproc)"
    make install
)

# 3. OpenSSL
OPENSSL_TARBALL="$DOWNLOAD_DIR/openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_SRC="$SRC_DIR/openssl-${OPENSSL_VERSION}"

download "$OPENSSL_URL" "$OPENSSL_TARBALL"
if [ ! -d "$OPENSSL_SRC" ]; then
    extract_tarball "$OPENSSL_TARBALL" "$SRC_DIR"
fi

log "Building OpenSSL ${OPENSSL_VERSION}"
(
    cd "$OPENSSL_SRC"
    ./Configure "$OPENSSL_TARGET" no-shared no-tests \
        --prefix="$PREFIX" \
        --openssldir="$PREFIX/ssl"
    make -j"$(nproc)"
    make install_sw
)

# 4. curl
CURL_TARBALL="$DOWNLOAD_DIR/curl-${CURL_VERSION}.tar.gz"
CURL_SRC="$SRC_DIR/curl-${CURL_VERSION}"

download "$CURL_URL" "$CURL_TARBALL"
if [ ! -d "$CURL_SRC" ]; then
    extract_tarball "$CURL_TARBALL" "$SRC_DIR"
fi

log "Building curl ${CURL_VERSION}"
(
    cd "$CURL_SRC"
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
    ./configure \
        --disable-shared \
        --enable-static \
        --prefix="$PREFIX" \
        --with-openssl="$PREFIX" \
        --with-zlib="$PREFIX" \
        --without-brotli \
        --without-zstd \
        --without-libpsl

    make -j"$(nproc)"
    make install
)

# 5. libarchive
LIBARCHIVE_TARBALL="$DOWNLOAD_DIR/libarchive-${LIBARCHIVE_VERSION}.tar.gz"
LIBARCHIVE_SRC="$SRC_DIR/libarchive-${LIBARCHIVE_VERSION}"
LIBARCHIVE_BUILD="$BUILD_DIR/libarchive-${LIBARCHIVE_VERSION}"

download "$LIBARCHIVE_URL" "$LIBARCHIVE_TARBALL"
if [ ! -d "$LIBARCHIVE_SRC" ]; then
    extract_tarball "$LIBARCHIVE_TARBALL" "$SRC_DIR"
fi

mkdir -p "$LIBARCHIVE_BUILD"

log "Building libarchive ${LIBARCHIVE_VERSION}"
(
    cd "$LIBARCHIVE_BUILD"
    cmake "$HOME/deps/src/libarchive-${LIBARCHIVE_VERSION}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$HOME/deps/install" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_INCLUDEDIR=include \
        -DCMAKE_PREFIX_PATH="$HOME/deps/install"

    cmake --build . -j"$(nproc)"
    cmake --install .
)

log "Done"