#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=package-common.sh
source "$SCRIPT_DIR/package-common.sh"

usage() {
    cat <<USAGE
Usage:
  $0 <version|stable|latest> <host_platform> <target_platform> <revision>

Examples:
  $0 stable linux-x64 linux-x64 1
  $0 stable linux-x64 windows-x64 1
  $0 stable windows-x64 windows-x64 1
USAGE
}

if [ "$#" -ne 4 ]; then
    usage >&2
    exit 2
fi

REQUESTED_VERSION="$1"
HOST_PLATFORM="$2"
TARGET_PLATFORM="$3"
REVISION="$4"

TOOL="gcc"
COMPONENT="compiler"
VERSION="$(resolve_version gcc "$REQUESTED_VERSION")"
PACKAGE_VERSION="$(package_version_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"
BINUTILS_VERSION="$(resolve_version binutils stable)"
MINGW_VERSION="$(resolve_version mingw stable)"

HOST_TRIPLE="$(platform_triple "$HOST_PLATFORM")"
TARGET_TRIPLE="$(platform_triple "$TARGET_PLATFORM")"
TARGET_FAMILY="$(platform_family "$TARGET_PLATFORM")"
TARGET_RUNTIME="$(platform_runtime "$TARGET_PLATFORM")"
THREAD_MODEL="$(platform_thread_model "$TARGET_PLATFORM")"

BUILD_ENVIRONMENT="${CUP_BUILD_ENVIRONMENT:-manual}"
SOURCE_POLICY="source-release"
PREFIX="$CUP_STAGE_DIR/$(package_base_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"
GCC_SOURCE_URL="$(source_url_gcc "$VERSION")"
BINUTILS_SOURCE_URL="$(source_url_binutils "$BINUTILS_VERSION")"
MINGW_SOURCE_URL="$(source_url_mingw "$MINGW_VERSION")"

PATH="$PREFIX/bin:$PATH"
export PATH

need_common_tools() {
    need curl
    need tar
    need make
    need zip

    if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        die "a host C compiler is required"
    fi
}

configure_and_build() {
    local source_dir="$1"
    local build_dir="$2"
    shift 2

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        "$source_dir/configure" "$@"
        make -j"$CUP_JOBS"
        make install
    )
}

build_native_gcc() {
    local gcc_src="$1"
    local build_dir="$CUP_BUILD_DIR/gcc-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "building native GCC $VERSION for $HOST_PLATFORM"

    (
        cd "$gcc_src"
        ./contrib/download_prerequisites
    )

    configure_and_build "$gcc_src" "$build_dir" \
        --prefix="$PREFIX" \
        --disable-werror \
        --enable-bootstrap \
        --enable-languages=c,c++
}

build_cross_binutils() {
    local binutils_src="$1"
    local build_dir="$CUP_BUILD_DIR/binutils-$BINUTILS_VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "building bundled Binutils $BINUTILS_VERSION for $TARGET_TRIPLE"

    configure_and_build "$binutils_src" "$build_dir" \
        --prefix="$PREFIX" \
        --target="$TARGET_TRIPLE" \
        --disable-werror
}

install_mingw_headers() {
    local mingw_src="$1"
    local headers_src="$mingw_src/mingw-w64-headers"
    local build_dir="$CUP_BUILD_DIR/mingw-headers-$MINGW_VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "installing bundled MinGW-w64 headers $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        "$headers_src/configure" \
            --host="$TARGET_TRIPLE" \
            --prefix="$PREFIX/$TARGET_TRIPLE" \
            --enable-sdk=all \
            --with-default-msvcrt=ucrt
        make install
    )
}

build_gcc_stage1() {
    local gcc_src="$1"
    local build_dir="$CUP_BUILD_DIR/gcc-stage1-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "building stage-1 GCC for $TARGET_TRIPLE"

    (
        cd "$gcc_src"
        ./contrib/download_prerequisites
    )

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        "$gcc_src/configure" \
            --prefix="$PREFIX" \
            --target="$TARGET_TRIPLE" \
            --disable-werror \
            --enable-languages=c,c++ \
            --enable-threads=posix \
            --with-gnu-as \
            --with-gnu-ld
        make -j"$CUP_JOBS" all-gcc
        make install-gcc
    )
}

build_mingw_crt() {
    local mingw_src="$1"
    local crt_src="$mingw_src/mingw-w64-crt"
    local build_dir="$CUP_BUILD_DIR/mingw-crt-$MINGW_VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "building bundled MinGW-w64 CRT $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        CC="$TARGET_TRIPLE-gcc" \
        AR="$TARGET_TRIPLE-ar" \
        RANLIB="$TARGET_TRIPLE-ranlib" \
        STRIP="$TARGET_TRIPLE-strip" \
        "$crt_src/configure" \
            --host="$TARGET_TRIPLE" \
            --prefix="$PREFIX/$TARGET_TRIPLE" \
            --with-default-msvcrt=ucrt
        make -j"$CUP_JOBS"
        make install
    )
}

build_winpthreads() {
    local mingw_src="$1"
    local pthreads_src="$mingw_src/mingw-w64-libraries/winpthreads"
    local build_dir="$CUP_BUILD_DIR/winpthreads-$MINGW_VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    if [ ! -d "$pthreads_src" ]; then
        log "winpthreads source directory not found; skipping"
        return 0
    fi

    log "building bundled winpthreads from MinGW-w64 $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        CC="$TARGET_TRIPLE-gcc" \
        AR="$TARGET_TRIPLE-ar" \
        RANLIB="$TARGET_TRIPLE-ranlib" \
        STRIP="$TARGET_TRIPLE-strip" \
        "$pthreads_src/configure" \
            --host="$TARGET_TRIPLE" \
            --prefix="$PREFIX/$TARGET_TRIPLE"
        make -j"$CUP_JOBS"
        make install
    )
}

build_gcc_final() {
    local gcc_src="$1"
    local build_dir="$CUP_BUILD_DIR/gcc-final-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    log "building final bundled GCC $VERSION for $TARGET_TRIPLE"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        "$gcc_src/configure" \
            --prefix="$PREFIX" \
            --target="$TARGET_TRIPLE" \
            --disable-werror \
            --enable-languages=c,c++ \
            --enable-threads=posix \
            --with-gnu-as \
            --with-gnu-ld
        make -j"$CUP_JOBS"
        make install
    )
}

build_bundled_windows_gcc() {
    local gcc_src="$1"
    local binutils_src="$2"
    local mingw_src="$3"

    log "building self-contained GCC package with bundled Binutils and MinGW-w64"

    build_cross_binutils "$binutils_src"
    install_mingw_headers "$mingw_src"
    build_gcc_stage1 "$gcc_src"
    build_mingw_crt "$mingw_src"
    build_winpthreads "$mingw_src"
    build_gcc_final "$gcc_src"
}

write_gcc_info() {
    local bundle_components=""
    local includes_binutils="false"
    local includes_mingw="false"
    local bootstrap="true"

    if is_windows_platform "$TARGET_PLATFORM"; then
        bundle_components="binutils,mingw-w64"
        includes_binutils="true"
        includes_mingw="true"
        bootstrap="false"
    fi

    local info=(
        "package.component=$COMPONENT"
        "package.tool=$TOOL"
        "package.version=$PACKAGE_VERSION"
        "package.revision=$REVISION"
        "package.mode=self-contained"
        "package.formats=$(package_formats_csv "$HOST_PLATFORM")"
        "platform.host=$HOST_PLATFORM"
        "platform.target=$TARGET_PLATFORM"
        "platform.host_triple=$HOST_TRIPLE"
        "platform.target_triple=$TARGET_TRIPLE"
        "platform.family=$TARGET_FAMILY"
        "platform.runtime=$TARGET_RUNTIME"
        "platform.thread_model=$THREAD_MODEL"
        "build.environment=$BUILD_ENVIRONMENT"
        "build.source_policy=$SOURCE_POLICY"
        "source.primary.name=gcc"
        "source.primary.version=$VERSION"
        "source.primary.url=$GCC_SOURCE_URL"
        "config.languages=c,c++"
        "config.bootstrap=$bootstrap"
        "contents.self_contained=true"
    )

    if [ -n "$bundle_components" ]; then
        info+=(
            "bundle.components=$bundle_components"
            "bundle.binutils.version=$BINUTILS_VERSION"
            "bundle.binutils.url=$BINUTILS_SOURCE_URL"
            "bundle.mingw-w64.version=$MINGW_VERSION"
            "bundle.mingw-w64.url=$MINGW_SOURCE_URL"
            "contents.includes_binutils=$includes_binutils"
            "contents.includes_mingw=$includes_mingw"
            "features.winpthreads=true"
        )
    fi

    write_info_file "$PREFIX" "${info[@]}"
}

main() {
    make_dirs
    need_common_tools
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"

    local gcc_src
    gcc_src="$(prepare_source_tree gcc "$VERSION" "$GCC_SOURCE_URL" "gcc-$VERSION.tar.xz")"

    if is_windows_platform "$TARGET_PLATFORM"; then
        local binutils_src
        local mingw_src

        binutils_src="$(prepare_source_tree binutils "$BINUTILS_VERSION" "$BINUTILS_SOURCE_URL" "binutils-$BINUTILS_VERSION.tar.xz")"
        mingw_src="$(prepare_source_tree mingw-w64 "$MINGW_VERSION" "$MINGW_SOURCE_URL" "mingw-w64-v$MINGW_VERSION.tar.bz2")"
        build_bundled_windows_gcc "$gcc_src" "$binutils_src" "$mingw_src"
    else
        if is_cross_build "$HOST_PLATFORM" "$TARGET_PLATFORM"; then
            die "unsupported GCC target: $HOST_PLATFORM -> $TARGET_PLATFORM"
        fi

        build_native_gcc "$gcc_src"
    fi

    write_gcc_info
    create_packages "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION" "$PREFIX"
}

main "$@"
