#!/usr/bin/env bash
set -euo pipefail

TOOL="${1:?missing tool}"
VERSION="${2:?missing version}"
BUILD_MODE="${3:?missing build mode}"

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"

case "${TOOL}" in
    gcc)
        exec bash "${ROOT_DIR}/scripts/build-gcc.sh" \
            "${VERSION}" \
            "${BUILD_MODE}"
        ;;

    gdb)
        exec bash "${ROOT_DIR}/scripts/build-gdb.sh" \
            "${VERSION}" \
            "${BUILD_MODE}"
        ;;

    *)
        echo "Error: unsupported GNU tool '${TOOL}'."
        exit 1
        ;;
esac