#!/usr/bin/env bash
set -euo pipefail

platform_arch="$(uname -m | sed 's/x86_64/x64/; s/arm64/arm64/')"
PLATFORM="${PLATFORM:-macos-${platform_arch}}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source "$SCRIPT_DIR/bootstrap-common.sh"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
PREFIX="${PREFIX:-$DEPS_ROOT/install}"
DEPS_SCOPE="${CUP_DEPS_SCOPE:-release}"

JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

case "$PLATFORM" in
    macos-x64)
        OPENSSL_TARGET="${OPENSSL_TARGET:-darwin64-x86_64-cc}"
        ;;
    macos-arm64)
        OPENSSL_TARGET="${OPENSSL_TARGET:-darwin64-arm64-cc}"
        ;;
    *)
        echo "Error: unsupported macOS dependency platform '$PLATFORM'." >&2
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
    LDFLAGS="-L$PREFIX/lib" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
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
    LDFLAGS="-L$PREFIX/lib" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
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

    curl_flags="$("$PREFIX/bin/curl-config" --static-libs)"
    archive_flags="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
        pkg-config --static --libs libarchive)"
    if [ -z "$curl_flags" ] || [ -z "$archive_flags" ]; then
        echo "Error: generated static link metadata is empty." >&2
        exit 1
    fi

    printf '%s\n' "$curl_flags"
    printf '%s\n' "$archive_flags"

    echo "==> macOS dependencies installed in $PREFIX"
}

main() {
    signature="$(dependency_signature "$PLATFORM" "${CC:-cc}")"
    signature="$signature
scope=$DEPS_SCOPE"
    require_tool "${CC:-cc}"
    require_tool "curl"
    require_tool "tar"
    require_tool "make"
    require_tool "mktemp"
    require_tool "perl"
    require_tool "pkg-config"
    require_sha256_tool
    prepare_dependency_prefix "$PREFIX" "$signature"
    mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX"

    if [ "$DEPS_SCOPE" = development ]; then
        build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
            "${CC:-cc}" "${AR:-ar}" "${RANLIB:-ranlib}"
        finish_dependency_prefix "$PREFIX"
        exit 0
    fi
    [ "$DEPS_SCOPE" = release ] || {
        echo "Error: CUP_DEPS_SCOPE must be development or release." >&2
        exit 1
    }

    build_zlib
    build_xz
    build_openssl
    build_curl
    build_libarchive
    build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "${CC:-cc}" "${AR:-ar}" "${RANLIB:-ranlib}"
    verify
    finish_dependency_prefix "$PREFIX"
}

main "$@"
