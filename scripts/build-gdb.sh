#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?missing version}"
BUILD_MODE="${2:?missing build mode}"

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"

SRC_ARCHIVE="gdb-${VERSION}.tar.xz"
SRC_URL="https://ftp.gnu.org/gnu/gdb/${SRC_ARCHIVE}"

SRC_DIR="${ROOT_DIR}/src"
BUILD_DIR="${ROOT_DIR}/build"
STAGE_DIR="${ROOT_DIR}/stage"
OUT_DIR="${ROOT_DIR}/out"

PACKAGE_NAME="gdb-${VERSION}-linux-x64-${BUILD_MODE}"
PREFIX_DIR="${STAGE_DIR}/${PACKAGE_NAME}"

BUILD_WORK_DIR="${BUILD_DIR}/gdb-${VERSION}-${BUILD_MODE}"
SOURCE_DIR="${SRC_DIR}/gdb-${VERSION}"

CONFIG_FLAGS=()

case "${BUILD_MODE}" in
    minimal)
        CONFIG_FLAGS+=(--disable-sim)
        CONFIG_FLAGS+=(--disable-gdbserver)
        ;;
    standard)
        CONFIG_FLAGS+=(--disable-sim)
        ;;
    full)
        ;;
    *)
        echo "Error: unknown GDB build mode '${BUILD_MODE}'."
        exit 1
        ;;
esac

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${STAGE_DIR}" "${OUT_DIR}"

echo "==> Tool: gdb"
echo "==> Version: ${VERSION}"
echo "==> Build mode: ${BUILD_MODE}"
echo "==> Package name: ${PACKAGE_NAME}"

echo "==> Cleaning previous build outputs"
rm -rf "${BUILD_WORK_DIR}" "${PREFIX_DIR}"
rm -f "${OUT_DIR}/${PACKAGE_NAME}.tar.gz"
rm -f "${OUT_DIR}/${PACKAGE_NAME}.tar.xz"

if [ ! -f "${SRC_DIR}/${SRC_ARCHIVE}" ]; then
    echo "==> Downloading GDB ${VERSION}"
    curl -fL "${SRC_URL}" -o "${SRC_DIR}/${SRC_ARCHIVE}"
else
    echo "==> Source archive already exists: ${SRC_ARCHIVE}"
fi

if [ ! -d "${SOURCE_DIR}" ]; then
    echo "==> Extracting GDB sources"
    tar -xJf "${SRC_DIR}/${SRC_ARCHIVE}" -C "${SRC_DIR}"
else
    echo "==> Source directory already exists: ${SOURCE_DIR}"
fi

echo "==> Configuring GDB"
mkdir -p "${BUILD_WORK_DIR}"
cd "${BUILD_WORK_DIR}"

"${SOURCE_DIR}/configure" \
    --prefix="${PREFIX_DIR}" \
    "${CONFIG_FLAGS[@]}"

echo "==> Building GDB"
make -j"$(nproc)"

echo "==> Installing GDB into staging directory"
make install

echo "==> Packaging GDB"
cd "${STAGE_DIR}"

tar -czf "${OUT_DIR}/${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
tar -cJf "${OUT_DIR}/${PACKAGE_NAME}.tar.xz" "${PACKAGE_NAME}"

echo "==> Done"
ls -lh "${OUT_DIR}/${PACKAGE_NAME}.tar.gz" "${OUT_DIR}/${PACKAGE_NAME}.tar.xz"