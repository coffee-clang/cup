#!/usr/bin/env bash

# Builds the complete Windows x64 dependency prefix natively. UCRT64
# is the production GCC graph; CLANG64 is an isolated diagnostic graph used by
# ASan/UBSan and is never reused for official release binaries.
set -euo pipefail

PLATFORM="windows-x64"
HOST_TRIPLE=x86_64-w64-mingw32

case "${MSYSTEM:-}" in
    UCRT64)
        if [ "${MINGW_PREFIX:-}" != /ucrt64 ]; then
            echo "Error: UCRT64 requires MINGW_PREFIX=/ucrt64." >&2
            exit 1
        fi
        TOOLCHAIN_LABEL=UCRT64
        CC=gcc
        AR=ar
        RANLIB=ranlib
        STRIP=strip
        WINDRES=windres
        DEFAULT_DEPS_VARIANT=windows-x64
        ;;
    CLANG64)
        if [ "${MINGW_PREFIX:-}" != /clang64 ]; then
            echo "Error: CLANG64 requires MINGW_PREFIX=/clang64." >&2
            exit 1
        fi
        TOOLCHAIN_LABEL=CLANG64
        CC=clang
        AR=llvm-ar
        RANLIB=llvm-ranlib
        STRIP=llvm-strip
        WINDRES=llvm-windres
        DEFAULT_DEPS_VARIANT=windows-x64-clang64
        ;;
    *)
        echo "Error: Windows dependencies require an MSYS2 UCRT64 or CLANG64 shell." >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"
dependency_normalize_build_environment
JOBS="$(dependency_resolve_jobs)"

DEPS_ROOT="${DEPS_ROOT:-$HOME/deps/$DEFAULT_DEPS_VARIANT}"
SRC_DIR="$DEPS_ROOT/src"
BUILD_DIR="$DEPS_ROOT/build"
DEPS_PREFIX="${DEPS_PREFIX:-$DEPS_ROOT/install}"
PREFIX="$DEPS_PREFIX"

dependency_require_whitespace_free_path "dependency root" "$DEPS_ROOT"
dependency_require_whitespace_free_path "dependency source directory" "$SRC_DIR"
dependency_require_whitespace_free_path "dependency build directory" "$BUILD_DIR"
dependency_require_whitespace_free_path "dependency prefix" "$DEPS_PREFIX"

# Static native Windows dependency builders.
build_zlib() {
    local archive
    local source

    archive="$SRC_DIR/zlib-${ZLIB_VERSION}.tar.gz"
    source="$BUILD_DIR/zlib-${ZLIB_VERSION}"

    download_source zlib "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building zlib ${ZLIB_VERSION} with MSYS2 ${TOOLCHAIN_LABEL}"
    cd "$source"

    make -f win32/Makefile.gcc clean

    make -f win32/Makefile.gcc \
        PREFIX= CC="$CC" AR="$AR" RC="$WINDRES" STRIP="$STRIP" \
        LOC="${CUP_DEPENDENCY_CFLAGS:-}" \
        -j"$JOBS" libz.a

    cup_path_copy_file "$PWD/zlib.h" "$PREFIX/include/zlib.h" 0644 replace
    cup_path_copy_file "$PWD/zconf.h" "$PREFIX/include/zconf.h" 0644 replace
    cup_path_copy_file "$PWD/libz.a" "$PREFIX/lib/libz.a" 0644 replace
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

    # shellcheck disable=SC2086
    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CFLAGS="$CUP_DEPENDENCY_CFLAGS" ./configure \
        --host="$HOST_TRIPLE" \
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

build_curl() {
    local archive
    local source

    archive="$SRC_DIR/curl-${CURL_VERSION}.tar.xz"
    source="$BUILD_DIR/curl-${CURL_VERSION}"

    download_source curl "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building curl ${CURL_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    # shellcheck disable=SC2086
    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CFLAGS="$CUP_DEPENDENCY_CFLAGS" CPPFLAGS="-I$PREFIX/include" \
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

    archive="$SRC_DIR/libarchive-${LIBARCHIVE_VERSION}.tar.xz"
    source="$BUILD_DIR/libarchive-${LIBARCHIVE_VERSION}"

    download_source libarchive "$archive"
    extract_archive "$archive" "$source"

    echo "==> Building libarchive ${LIBARCHIVE_VERSION} for ${HOST_TRIPLE}"
    cd "$source"

    # shellcheck disable=SC2086
    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" WINDRES="$WINDRES" \
    CFLAGS="$CUP_DEPENDENCY_CFLAGS" CPPFLAGS="-I$PREFIX/include" \
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
        --without-openssl \
        --without-iconv \
        --without-libiconv-prefix

    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

# Final prefix and static metadata verification.
verify() {
    echo "==> Verifying generated dependency prefix"
    dependency_prefix_complete "$PREFIX" 0 "$CUP_DEPS_FINAL_PREFIX" || {
        echo "Error: generated dependency prefix is incomplete or unsafe." >&2
        exit 1
    }
    echo "==> Windows dependencies verified for $CUP_DEPS_FINAL_PREFIX"
}

# Ordered Windows x64 bootstrap.
main() {
    local compiler_target
    local profile
    local metadata

    require_tool "$CC"
    require_tool "$AR"
    require_tool "$RANLIB"
    require_tool "$STRIP"
    require_tool "$WINDRES"
    require_tool cmp
    require_sha256_tool
    compiler_target=$("$CC" -dumpmachine)
    case "$compiler_target" in
        x86_64-w64-mingw32|x86_64-w64-windows-gnu) ;;
        *)
            echo "Error: $TOOLCHAIN_LABEL compiler target '$compiler_target' is unsupported." >&2
            exit 1
            ;;
    esac
    profile=$(dependency_profile "$PLATFORM")
    metadata=$(dependency_metadata "$PLATFORM" "$profile")
    dependency_acquire_build_lock "$DEPS_ROOT"
    trap 'abort_dependency_prefix; dependency_release_build_lock' EXIT
    prepare_dependency_prefix "$DEPS_PREFIX" "$metadata" 0
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
    cup_path_prepare_child_directory "$DEPS_ROOT" "$PREFIX/bin" \
        "dependency binary directory"
    cup_path_prepare_child_directory "$DEPS_ROOT" "$PREFIX/include" \
        "dependency include directory"
    cup_path_prepare_child_directory "$DEPS_ROOT" "$PREFIX/lib" \
        "dependency library directory"

    build_zlib
    build_xz
    build_cares_static "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB" "$HOST_TRIPLE" \
        "-lws2_32 -ladvapi32 -liphlpapi"
    build_curl
    build_libarchive
    build_libevent_static "$SRC_DIR" "$BUILD_DIR" \
        "$CC" "$AR" "$RANLIB" "$HOST_TRIPLE"
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
