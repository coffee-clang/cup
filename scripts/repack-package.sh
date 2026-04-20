#!/usr/bin/env bash
set -euo pipefail

PACKAGE_NAME="${1:?missing package name}"
SOURCE_ARCHIVE="${2:?missing source archive}"
OUTPUT_ARCHIVE="${3:?missing output archive}"

WORK_DIR="$(mktemp -d)"
EXTRACT_DIR="${WORK_DIR}/extract"

cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

mkdir -p "${EXTRACT_DIR}"

case "${SOURCE_ARCHIVE}" in
    *.tar.gz)
        tar -xzf "${SOURCE_ARCHIVE}" -C "${EXTRACT_DIR}"
        ;;
    *.tar.xz)
        tar -xJf "${SOURCE_ARCHIVE}" -C "${EXTRACT_DIR}"
        ;;
    *)
        echo "Error: unsupported source archive format '${SOURCE_ARCHIVE}'."
        exit 1
        ;;
esac

if [ ! -d "${EXTRACT_DIR}/${PACKAGE_NAME}" ]; then
    echo "Error: expected package root '${PACKAGE_NAME}' not found after extraction."
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT_ARCHIVE}")"

case "${OUTPUT_ARCHIVE}" in
    *.tar.gz)
        tar -czf "${OUTPUT_ARCHIVE}" -C "${EXTRACT_DIR}" "${PACKAGE_NAME}"
        ;;
    *.tar.xz)
        tar -cJf "${OUTPUT_ARCHIVE}" -C "${EXTRACT_DIR}" "${PACKAGE_NAME}"
        ;;
    *)
        echo "Error: unsupported output archive format '${OUTPUT_ARCHIVE}'."
        exit 1
        ;;
esac

echo "Created ${OUTPUT_ARCHIVE}"