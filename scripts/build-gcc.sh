#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?missing version}"
BUILD_MODE="${2:?missing build mode}"

ROOT_DIR="/work"
SRC_ARCHIVE="gcc-${VERSION}.tar.gz"
SRC_URL="https://gcc.gnu.org/pub/gcc/releases/gcc-${VERSION}/${SRC_ARCHIVE}"

SRC_DIR="${ROOT_DIR}/src"
BUILD_DIR="${ROOT_DIR}/build"
STAGE_DIR="${ROOT_DIR}/stage"
OUT_DIR="${ROOT_DIR}/out"

PACKAGE_NAME="gcc-${VERSION}-linux-x64-${BUILD_MODE}"
PREFIX_DIR="${STAGE_DIR}/${PACKAGE_NAME}"

CONFIG_FLAGS=()

case "${BUILD_MODE}" in
  minimal)
    CONFIG_FLAGS+=(--enable-languages=c,c++)
    CONFIG_FLAGS+=(--disable-bootstrap)
    CONFIG_FLAGS+=(--disable-multilib)
    ;;
  standard)
    CONFIG_FLAGS+=(--disable-bootstrap)
    CONFIG_FLAGS+=(--disable-multilib)
    ;;
  full)
    CONFIG_FLAGS+=(--disable-multilib)
    ;;
  *)
    echo "Error: unknown build mode '${BUILD_MODE}'"
    exit 1
    ;;
esac

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${STAGE_DIR}" "${OUT_DIR}"

echo "==> Build mode: ${BUILD_MODE}"
echo "==> GCC version: ${VERSION}"

echo "==> Downloading GCC ${VERSION}"
curl -L "${SRC_URL}" -o "${SRC_DIR}/${SRC_ARCHIVE}"

echo "==> Extracting sources"
tar -xzf "${SRC_DIR}/${SRC_ARCHIVE}" -C "${SRC_DIR}"

echo "==> Downloading GCC prerequisites"
cd "${SRC_DIR}/gcc-${VERSION}"
./contrib/download_prerequisites

echo "==> Configuring build"
mkdir -p "${BUILD_DIR}/gcc-${VERSION}-${BUILD_MODE}"
cd "${BUILD_DIR}/gcc-${VERSION}-${BUILD_MODE}"

"${SRC_DIR}/gcc-${VERSION}/configure" \
    --prefix="${PREFIX_DIR}" \
    "${CONFIG_FLAGS[@]}"

echo "==> Building"
make -j"$(nproc)"

echo "==> Installing into staging directory"
make install

echo "==> Packaging release archive"
cd "${STAGE_DIR}"
tar -czf "${OUT_DIR}/${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"

echo "==> Done"
ls -lh "${OUT_DIR}"
