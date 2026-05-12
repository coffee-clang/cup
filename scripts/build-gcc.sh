#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
    need realpath

    if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        die "a host C compiler is required"
    fi
}

configure_script_for_build() {
    local source_dir="$1"
    local build_dir="$2"
    local source_ref

    if [ "$HOST_PLATFORM" = "windows-x64" ]; then
        source_ref="$(realpath --relative-to="$build_dir" "$source_dir")"
    else
        source_ref="$source_dir"
    fi

    printf '%s/configure\n' "$source_ref"
}

target_tool_path() {
    local tool="$1"
    local exe_suffix=""

    if [ "$HOST_PLATFORM" = "windows-x64" ]; then
        exe_suffix=".exe"
    fi

    printf '%s/%s-%s%s\n' "$PREFIX/bin" "$TARGET_TRIPLE" "$tool" "$exe_suffix"
}

log_target_tools() {
    log "using bundled target tools:"
    log "  CC=$(target_tool_path gcc)"
    log "  AR=$(target_tool_path ar)"
    log "  RANLIB=$(target_tool_path ranlib)"
    log "  STRIP=$(target_tool_path strip)"
}

prepare_gcc_prerequisites() {
    local gcc_src="$1"

    if [ "$HOST_PLATFORM" = "windows-x64" ]; then
        log "using MSYS2 packaged GCC prerequisites on Windows host"
        return 0
    fi

    log "downloading GCC prerequisites with contrib/download_prerequisites"

    (
        cd "$gcc_src"
        ./contrib/download_prerequisites
    )
}

gcc_dependency_configure_args() {
    if [ "$HOST_PLATFORM" = "windows-x64" ]; then
        if [ -z "${MINGW_PREFIX:-}" ]; then
            die "MINGW_PREFIX is not set; run this build inside an MSYS2 MinGW/UCRT environment"
        fi

        printf '%s\n' \
            --with-gmp="$MINGW_PREFIX" \
            --with-mpfr="$MINGW_PREFIX" \
            --with-mpc="$MINGW_PREFIX" \
            --with-isl="$MINGW_PREFIX"
    fi
}

gcc_windows_target_configure_args() {
    if is_windows_platform "$TARGET_PLATFORM"; then
        printf '%s\n' \
            --with-sysroot="$PREFIX/$TARGET_TRIPLE" \
            --with-build-sysroot="$PREFIX/$TARGET_TRIPLE" \
            --with-native-system-header-dir=/include
    fi
}

configure_and_build() {
    local source_dir="$1"
    local build_dir="$2"
    local configure_script

    shift 2

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$source_dir" "$build_dir")"

    (
        cd "$build_dir"
        "$configure_script" "$@"
        make -j"$CUP_JOBS"
        make install
    )
}

build_native_gcc() {
    local gcc_src="$1"
    local build_dir="$CUP_BUILD_DIR/gcc-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"
    local gcc_dep_args=()

    log "building native GCC $VERSION for $HOST_PLATFORM"

    mapfile -t gcc_dep_args < <(gcc_dependency_configure_args)

    prepare_gcc_prerequisites "$gcc_src"

    configure_and_build "$gcc_src" "$build_dir" \
        --prefix="$PREFIX" \
        --disable-werror \
        --disable-multilib \
        --enable-bootstrap \
        --enable-languages=c,c++ \
        "${gcc_dep_args[@]}"
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
    local configure_script

    log "installing bundled MinGW-w64 headers $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$headers_src" "$build_dir")"

    (
        cd "$build_dir"
        "$configure_script" \
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
    local configure_script
    local gcc_dep_args=()
    local gcc_target_args=()

    log "building stage-1 GCC for $TARGET_TRIPLE"

    mapfile -t gcc_dep_args < <(gcc_dependency_configure_args)
    mapfile -t gcc_target_args < <(gcc_windows_target_configure_args)

    prepare_gcc_prerequisites "$gcc_src"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$gcc_src" "$build_dir")"

    (
        cd "$build_dir"
        "$configure_script" \
            --prefix="$PREFIX" \
            --target="$TARGET_TRIPLE" \
            --disable-werror \
            --disable-multilib \
            --enable-languages=c,c++ \
            --enable-threads=posix \
            --with-gnu-as \
            --with-gnu-ld \
            "${gcc_dep_args[@]}" \
            "${gcc_target_args[@]}"
        make -j"$CUP_JOBS" all-gcc
        make install-gcc
    )
}

build_mingw_crt() {
    local mingw_src="$1"
    local crt_src="$mingw_src/mingw-w64-crt"
    local build_dir="$CUP_BUILD_DIR/mingw-crt-$MINGW_VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"
    local configure_script

    log "building bundled MinGW-w64 CRT $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$crt_src" "$build_dir")"

    log_target_tools

    (
        cd "$build_dir"
        CC="$(target_tool_path gcc)" \
        AR="$(target_tool_path ar)" \
        RANLIB="$(target_tool_path ranlib)" \
        STRIP="$(target_tool_path strip)" \
        "$configure_script" \
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
    local configure_script

    if [ ! -d "$pthreads_src" ]; then
        log "winpthreads source directory not found; skipping"
        return 0
    fi

    log "building bundled winpthreads from MinGW-w64 $MINGW_VERSION"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$pthreads_src" "$build_dir")"

    log_target_tools

    (
        cd "$build_dir"
        CC="$(target_tool_path gcc)" \
        AR="$(target_tool_path ar)" \
        RANLIB="$(target_tool_path ranlib)" \
        STRIP="$(target_tool_path strip)" \
        "$configure_script" \
            --host="$TARGET_TRIPLE" \
            --prefix="$PREFIX/$TARGET_TRIPLE"
        make -j"$CUP_JOBS"
        make install
    )
}

build_gcc_final() {
    local gcc_src="$1"
    local build_dir="$CUP_BUILD_DIR/gcc-final-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"
    local configure_script
    local gcc_dep_args=()
    local gcc_target_args=()

    log "building final bundled GCC $VERSION for $TARGET_TRIPLE"

    mapfile -t gcc_dep_args < <(gcc_dependency_configure_args)
    mapfile -t gcc_target_args < <(gcc_windows_target_configure_args)

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    configure_script="$(configure_script_for_build "$gcc_src" "$build_dir")"

    (
        cd "$build_dir"
        "$configure_script" \
            --prefix="$PREFIX" \
            --target="$TARGET_TRIPLE" \
            --disable-werror \
            --disable-multilib \
            --enable-languages=c,c++ \
            --enable-threads=posix \
            --with-gnu-as \
            --with-gnu-ld \
            "${gcc_dep_args[@]}" \
            "${gcc_target_args[@]}"
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
        "config.multilib=false"
        "config.bootstrap=$bootstrap"
        "contents.self_contained=true"
    )

    if [ "$HOST_PLATFORM" = "windows-x64" ]; then
        info+=(
            "build.gcc_prerequisites=msys2"
        )
    else
        info+=(
            "build.gcc_prerequisites=contrib-download_prerequisites"
        )
    fi

    if is_windows_platform "$TARGET_PLATFORM"; then
        info+=(
            "config.sysroot=$TARGET_TRIPLE"
            "config.native_system_header_dir=/include"
        )
    fi

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
    local gcc_src

    make_dirs
    need_common_tools

    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"

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