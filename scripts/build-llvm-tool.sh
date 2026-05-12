#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/package-common.sh"

usage() {
    cat <<USAGE
Usage:
  $0 <clang|lld|lldb> <version|stable|latest> <host_platform> <target_platform> <revision>

Examples:
  $0 clang stable linux-x64 linux-x64 1
  $0 lld stable windows-x64 windows-x64 1
  $0 lldb stable windows-x64 windows-x64 1
USAGE
}

if [ "$#" -ne 5 ]; then
    usage >&2
    exit 2
fi

TOOL="$1"
REQUESTED_VERSION="$2"
HOST_PLATFORM="$3"
TARGET_PLATFORM="$4"
REVISION="$5"

VERSION="$(resolve_version llvm "$REQUESTED_VERSION")"
PACKAGE_VERSION="$(package_version_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"
HOST_TRIPLE="$(platform_triple "$HOST_PLATFORM")"
TARGET_TRIPLE="$(platform_triple "$TARGET_PLATFORM")"
TARGET_FAMILY="$(platform_family "$TARGET_PLATFORM")"
TARGET_RUNTIME="$(platform_runtime "$TARGET_PLATFORM")"
THREAD_MODEL="$(platform_thread_model "$TARGET_PLATFORM")"
BUILD_ENVIRONMENT="${CUP_BUILD_ENVIRONMENT:-manual}"
SOURCE_POLICY="source-release"
SOURCE_URL="$(source_url_llvm_project "$VERSION")"

case "$TOOL" in
    clang)
        COMPONENT="compiler"
        LLVM_PROJECTS="clang;lld"
        CONTENTS_EXTRA=("contents.includes_lld=true")
        ;;
    lld)
        COMPONENT="linker"
        LLVM_PROJECTS="lld"
        CONTENTS_EXTRA=()
        ;;
    lldb)
        COMPONENT="debugger"
        LLVM_PROJECTS="clang;lld;lldb"
        CONTENTS_EXTRA=("contents.includes_clang=true" "contents.includes_lld=true")
        ;;
    *)
        die "unsupported LLVM tool: $TOOL"
        ;;
esac

PREFIX="$CUP_STAGE_DIR/$(package_base_name "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION")"

need_common_tools() {
    need curl
    need tar
    need cmake
    need ninja
    need zip
}

build_llvm_tool() {
    local source_dir="$1"
    local build_dir="$CUP_BUILD_DIR/llvm-$TOOL-$VERSION-$HOST_PLATFORM-$TARGET_PLATFORM"
    local cmake_extra_args=()

    if is_cross_build "$HOST_PLATFORM" "$TARGET_PLATFORM"; then
        die "cross LLVM tool builds are not supported by this recipe yet: $HOST_PLATFORM -> $TARGET_PLATFORM"
    fi

    if [ "$TOOL" = "lldb" ]; then
        cmake_extra_args+=(
            -DLLDB_ENABLE_PYTHON=ON
            -DLLDB_ENABLE_LIBXML2=ON
            -DLLDB_ENABLE_LZMA=ON
        )

        if [ "$HOST_PLATFORM" != "windows-x64" ]; then
            cmake_extra_args+=(
                -DLLDB_ENABLE_LIBEDIT=ON
                -DLLDB_ENABLE_CURSES=ON
            )
        fi
    fi

    log "building LLVM tool $TOOL $VERSION with projects: $LLVM_PROJECTS"

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    cmake -S "$source_dir/llvm" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DLLVM_ENABLE_PROJECTS="$LLVM_PROJECTS" \
        -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLDB_INCLUDE_TESTS=OFF \
        "${cmake_extra_args[@]}"

    cmake --build "$build_dir" --parallel "$CUP_JOBS"
    cmake --install "$build_dir"
}

write_llvm_info() {
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
        "source.primary.name=llvm-project"
        "source.primary.version=$VERSION"
        "source.primary.url=$SOURCE_URL"
        "config.llvm_projects=$LLVM_PROJECTS"
        "config.llvm_targets=X86"
        "contents.self_contained=true"
    )

    info+=("${CONTENTS_EXTRA[@]}")

    write_info_file "$PREFIX" "${info[@]}"
}

main() {
    make_dirs
    need_common_tools
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX"

    local source_dir
    source_dir="$(prepare_source_tree llvm-project "$VERSION" "$SOURCE_URL" "llvm-project-$VERSION.src.tar.xz")"

    build_llvm_tool "$source_dir"
    write_llvm_info
    create_packages "$TOOL" "$VERSION" "$HOST_PLATFORM" "$TARGET_PLATFORM" "$REVISION" "$PREFIX"
}

main "$@"
