#!/usr/bin/env bash
set -euo pipefail

PLATFORM="windows-x64"
HOST_TRIPLE="${HOST_TRIPLE:-x86_64-w64-mingw32}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source "$SCRIPT_DIR/bootstrap-common.sh"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
PREFIX="${PREFIX:-$DEPS_ROOT/install}"

JOBS="${JOBS:-$(nproc)}"

CC="${CC:-${HOST_TRIPLE}-gcc}"
AR="${AR:-${HOST_TRIPLE}-ar}"
RANLIB="${RANLIB:-${HOST_TRIPLE}-ranlib}"
STRIP="${STRIP:-${HOST_TRIPLE}-strip}"
WINDRES="${WINDRES:-${HOST_TRIPLE}-windres}"

build_zlib() {
    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download_source zlib "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building zlib ${ZLIB_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    make -f win32/Makefile.gcc clean

    make -f win32/Makefile.gcc \
        PREFIX="${HOST_TRIPLE}-" \
        -j"$JOBS"

    make -f win32/Makefile.gcc \
        PREFIX="${HOST_TRIPLE}-" \
        BINARY_PATH="$PREFIX/bin" \
        INCLUDE_PATH="$PREFIX/include" \
        LIBRARY_PATH="$PREFIX/lib" \
        install
}

build_xz() {
    archive="$SRC_DIR/xz-${XZ_VERSION}.tar.xz"
    source="$BUILD_DIR/xz-${XZ_VERSION}"

    download_source xz "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building xz ${XZ_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    ./configure \
        --host="$HOST_TRIPLE" \
        --prefix="$PREFIX" \
        --disable-shared \
        --enable-static

    make -j"$JOBS"
    make install
}

build_curl() {
    archive="$SRC_DIR/curl-${CURL_VERSION}.tar.xz"
    source="$BUILD_DIR/curl-${CURL_VERSION}"

    download_source curl "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building curl ${CURL_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64" \
    LIBS="-lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    ./configure \
        --host="$HOST_TRIPLE" \
        --prefix="$PREFIX" \
        --disable-shared \
        --enable-static \
        --with-schannel \
        --without-openssl \
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

    echo "==> Building libarchive ${LIBARCHIVE_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" WINDRES="$WINDRES" \
    CPPFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64" \
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    ./configure \
        --host="$HOST_TRIPLE" \
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
    archive_flags="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
        pkg-config --static --libs libarchive)"
    if [ -z "$curl_flags" ] || [ -z "$archive_flags" ]; then
        echo "Error: generated static link metadata is empty." >&2
        exit 1
    fi

    printf '%s\n' "$curl_flags"
    printf '%s\n' "$archive_flags"

    echo "==> Windows dependencies installed in $PREFIX"
}

main() {
    signature="$(dependency_signature "$PLATFORM" "$CC|$AR|$RANLIB" 0)"
    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_tool "$WINDRES"
    require_tool curl
    require_tool tar
    require_tool make
    require_tool mktemp
    require_tool perl
    require_tool pkg-config
    require_sha256_tool
    prepare_dependency_prefix "$PREFIX" "$signature"

    mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX" "$PREFIX/bin" "$PREFIX/include" "$PREFIX/lib"

    build_zlib
    build_xz
    build_curl
    build_libarchive
    build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    verify
    finish_dependency_prefix "$PREFIX"
}

main "$@"
