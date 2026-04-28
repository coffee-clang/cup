#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?missing version}"
PLATFORM="${2:?missing platform}"

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"

SRC_DIR="${ROOT_DIR}/src"
BUILD_DIR="${ROOT_DIR}/build"
STAGE_DIR="${ROOT_DIR}/stage"
OUT_DIR="${ROOT_DIR}/out"

PACKAGE_NAME="lldb-${VERSION}-${PLATFORM}"
PREFIX_DIR="${STAGE_DIR}/${PACKAGE_NAME}"
BUILD_WORK_DIR="${BUILD_DIR}/lldb-${VERSION}-${PLATFORM}"
SOURCE_DIR="${SRC_DIR}/llvm-project-${VERSION}.src"

prepare_llvm_source() {
    local version="$1"
    local src_dir="$2"

    local archive="llvm-project-${version}.src.tar.xz"
    local url="https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/${archive}"
    local source_dir="${src_dir}/llvm-project-${version}.src"

    mkdir -p "${src_dir}"

    if [ ! -f "${src_dir}/${archive}" ]; then
        echo "==> Downloading LLVM project ${version}"
        curl -fL "${url}" -o "${src_dir}/${archive}"
    else
        echo "==> Source archive already exists: ${archive}"
    fi

    if [ ! -d "${source_dir}" ]; then
        echo "==> Extracting LLVM sources"
        tar -xJf "${src_dir}/${archive}" -C "${src_dir}"
    else
        echo "==> Source directory already exists: ${source_dir}"
    fi

    if [ ! -d "${source_dir}/llvm" ]; then
        echo "Error: LLVM source root not found at '${source_dir}/llvm'."
        exit 1
    fi
}

llvm_targets_for_platform() {
    local platform="$1"

    case "${platform}" in
        linux-x64)
            printf '%s\n' "X86"
            ;;
        *)
            echo "Error: unsupported platform '${platform}'."
            exit 1
            ;;
    esac
}

package_stage() {
    local package_name="$1"
    local stage_dir="$2"
    local out_dir="$3"

    mkdir -p "${out_dir}"

    echo "==> Packaging ${package_name}"
    cd "${stage_dir}"

    tar -czf "${out_dir}/${package_name}.tar.gz" "${package_name}"
    tar -cJf "${out_dir}/${package_name}.tar.xz" "${package_name}"

    echo "==> Done"
    ls -lh "${out_dir}/${package_name}.tar.gz" "${out_dir}/${package_name}.tar.xz"
}

LLVM_TARGETS_TO_BUILD="$(llvm_targets_for_platform "${PLATFORM}")"

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${STAGE_DIR}" "${OUT_DIR}"

echo "==> Tool: lldb"
echo "==> Version: ${VERSION}"
echo "==> Platform: ${PLATFORM}"
echo "==> Package name: ${PACKAGE_NAME}"
echo "==> LLVM targets: ${LLVM_TARGETS_TO_BUILD}"

echo "==> Cleaning previous build outputs"
rm -rf "${BUILD_WORK_DIR}" "${PREFIX_DIR}"
rm -f "${OUT_DIR}/${PACKAGE_NAME}.tar.gz"
rm -f "${OUT_DIR}/${PACKAGE_NAME}.tar.xz"

prepare_llvm_source "${VERSION}" "${SRC_DIR}"

echo "==> Configuring LLDB"
cmake -S "${SOURCE_DIR}/llvm" \
      -B "${BUILD_WORK_DIR}" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PREFIX_DIR}" \
      -DLLVM_ENABLE_PROJECTS="clang;lldb" \
      -DLLVM_TARGETS_TO_BUILD="${LLVM_TARGETS_TO_BUILD}" \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_BENCHMARKS=OFF \
      -DLLVM_INCLUDE_EXAMPLES=OFF \
      -DLLVM_ENABLE_ASSERTIONS=OFF

echo "==> Building LLDB"
cmake --build "${BUILD_WORK_DIR}" --parallel "$(nproc)"

echo "==> Installing LLDB into staging directory"
cmake --install "${BUILD_WORK_DIR}"

package_stage "${PACKAGE_NAME}" "${STAGE_DIR}" "${OUT_DIR}"
