#!/usr/bin/env bash

# Purpose: Builds the complete Windows x64 dependency prefix natively with the
# canonical MSYS2 UCRT64 toolchain and Schannel TLS backend.
set -euo pipefail

PLATFORM="windows-x64"
HOST_TRIPLE=x86_64-w64-mingw32
RUNTIME_POLICY=msys2-ucrt64

[ "${MSYSTEM:-}" = UCRT64 ] || {
    echo "Error: Windows dependencies require an MSYS2 UCRT64 shell." >&2
    exit 1
}
[ "${MINGW_PREFIX:-}" = /ucrt64 ] || {
    echo "Error: Windows dependencies require MINGW_PREFIX=/ucrt64." >&2
    exit 1
}

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/common.sh"
dependency_normalize_build_environment
JOBS="$(dependency_resolve_jobs)"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$PLATFORM}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
DEPS_PREFIX="${DEPS_PREFIX:-$DEPS_ROOT/install}"
PREFIX="$DEPS_PREFIX"


CC=gcc
AR=ar
RANLIB=ranlib
STRIP=strip
WINDRES=windres

# Static native UCRT64 dependency builders.
build_zlib() {
    local archive
    local source

    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download_source zlib "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building zlib ${ZLIB_VERSION} with MSYS2 UCRT64"
    cd "$source"

    make -f win32/Makefile.gcc clean

    make -f win32/Makefile.gcc \
        PREFIX= \
        -j"$JOBS" \
        libz.a

    cp zlib.h zconf.h "$PREFIX/include/"
    cp libz.a "$PREFIX/lib/libz.a"
}

build_xz() {
    local archive
    local source

    archive="$SRC_DIR/xz-${XZ_VERSION}.tar.xz"
    source="$BUILD_DIR/xz-${XZ_VERSION}"

    download_source xz "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building xz ${XZ_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    ./configure \
        --host="$HOST_TRIPLE" \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

build_curl() {
    local archive
    local source

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
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="" \
    ./configure \
        --host="$HOST_TRIPLE" \
        --prefix="$INSTALL_PREFIX" \
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
    make install DESTDIR="$DESTDIR"
}

build_libarchive() {
    local archive
    local source

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
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="" \
    ./configure \
        --host="$HOST_TRIPLE" \
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
    local curl_flags
    local archive_flags

    echo "==> Verifying generated link metadata"

    if [ ! -x "$PREFIX/bin/curl-config" ]; then
        echo "Error: curl-config was not installed." >&2
        exit 1
    fi

    curl_flags="$("$PREFIX/bin/curl-config" --static-libs)"
    archive_flags="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
        PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="" \
        pkg-config --static --libs libarchive)"
    if [ -z "$curl_flags" ] || [ -z "$archive_flags" ]; then
        echo "Error: generated static link metadata is empty." >&2
        exit 1
    fi
    if [ -n "$CUP_DEPS_STAGE_ROOT" ]; then
        case "$curl_flags $archive_flags" in
            *"$CUP_DEPS_STAGE_ROOT"*)
                echo "Error: generated link metadata contains the staging path." >&2
                exit 1
                ;;
        esac
    fi

    if ! dependency_prefix_complete "$PREFIX" 0 "$CUP_DEPS_FINAL_PREFIX"; then
        echo "Error: generated dependency prefix is incomplete." >&2
        exit 1
    fi

    printf '%s\n' "$curl_flags"
    printf '%s\n' "$archive_flags"

    echo "==> Windows dependencies verified for $CUP_DEPS_FINAL_PREFIX"
}

# Ordered Windows x64 bootstrap.
main() {
    local compiler_target
    local toolchain
    local id
    local metadata

    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_tool "$STRIP"
    require_tool "$WINDRES"
    require_sha256_tool
    compiler_target=$("$CC" -dumpmachine)
    [ "$compiler_target" = "$HOST_TRIPLE" ] || {
        echo "Error: UCRT64 compiler target is '$compiler_target', expected '$HOST_TRIPLE'." >&2
        exit 1
    }
    toolchain=$(dependency_windows_toolchain_identity \
        "$HOST_TRIPLE" "$CC" "$AR" "$RANLIB" "$STRIP" "$WINDRES" \
        "$RUNTIME_POLICY")
    id=$(dependency_id "$PLATFORM" "$toolchain" 0 "$PROJECT_ROOT" \
        "$SCRIPT_DIR/sources.sh" \
        "$SCRIPT_DIR/common.sh" \
        "$SCRIPT_DIR/build-windows.sh")
    metadata=$(dependency_metadata "$PLATFORM" "$id")
    prepare_dependency_prefix "$DEPS_PREFIX" "$metadata" 0
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

    mkdir -p "$SRC_DIR" "$BUILD_DIR" "$PREFIX" "$PREFIX/bin" "$PREFIX/include" "$PREFIX/lib"

    build_zlib
    build_xz
    build_curl
    build_libarchive
    build_argtable3_uthash_unity "$PREFIX" "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB"
    normalize_dependency_metadata "$PREFIX" \
        "$CUP_DEPS_BUILD_PREFIX" "$CUP_DEPS_FINAL_PREFIX"
    verify
    finish_dependency_prefix "$PREFIX"
    trap - EXIT HUP INT TERM
}

main "$@"
