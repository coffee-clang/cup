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

TOOL="gdb"
COMPONENT="debugger"
VERSION="$(resolve_version gdb "$REQUESTED_VERSION")"
PACKAGE_VERSION="$(package_version_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"
HOST_TRIPLE="$(platform_triple "$HOST_PLATFORM")"
TARGET_TRIPLE="$(platform_triple "$TARGET_PLATFORM")"
TARGET_FAMILY="$(platform_family "$TARGET_PLATFORM")"
TARGET_RUNTIME="$(platform_runtime "$TARGET_PLATFORM")"
THREAD_MODEL="$(platform_thread_model "$TARGET_PLATFORM")"
BUILD_ENVIRONMENT="${CUP_BUILD_ENVIRONMENT:-manual}"
SOURCE_POLICY="source-release"
PREFIX="$CUP_STAGE_DIR/$(package_base_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"
SOURCE_URL="$(source_url_gdb "$VERSION")"

need_common_tools() {
    need curl
    need tar
    need make
    need zip
    need python3

    if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        die "a host C compiler is required"
    fi
}

build_gdb() {
    local source_dir="$1"
    local build_dir="$CUP_BUILD_DIR/gdb-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"

    if is_cross_build "$HOST_PLATFORM" "$TARGET_PLATFORM"; then
        die "cross GDB is not supported by this build recipe yet: $HOST_PLATFORM -> $TARGET_PLATFORM"
    fi

    log "building GDB $VERSION for $HOST_PLATFORM"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    (
        cd "$build_dir"
        "$source_dir/configure" \
            --prefix="$PREFIX" \
            --disable-werror \
            --with-python=python3 \
            --with-expat \
            --with-system-readline \
            --with-zlib \
            --with-lzma \
            --with-zstd
        make -j"$CUP_JOBS"
        make install
    )

    if is_windows_platform "$HOST_PLATFORM"; then
        copy_windows_python_runtime
        copy_windows_runtime_dlls "$PREFIX/bin"
    fi
}

write_gdb_info() {
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
        "source.primary.name=gdb"
        "source.primary.version=$VERSION"
        "source.primary.url=$SOURCE_URL"
        "config.cross=false"
        "contents.self_contained=true"
        "contents.uses_python=true"
        "contents.uses_readline=true"
        "contents.uses_expat=true"
        "contents.uses_zlib=true"
        "contents.uses_lzma=true"
        "contents.uses_zstd=true"
    )

    write_info_file "$PREFIX" "${info[@]}"
}

main() {
    make_dirs
    need_common_tools
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"

    local source_dir
    source_dir="$(prepare_source_tree gdb "$VERSION" "$SOURCE_URL" "gdb-$VERSION.tar.xz")"

    build_gdb "$source_dir"
    write_gdb_info
    create_packages "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION" "$PREFIX"
}

main "$@"
