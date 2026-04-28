#!/usr/bin/env bash
set -euo pipefail

TOOL="${1:?missing tool}"
VERSION="${2:?missing version}"
PLATFORM="${3:?missing platform}"

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"

case "${TOOL}" in
    clang)
        exec bash "${ROOT_DIR}/scripts/build-clang.sh" \
            "${VERSION}" \
            "${PLATFORM}"
        ;;

    lldb)
        exec bash "${ROOT_DIR}/scripts/build-lldb.sh" \
            "${VERSION}" \
            "${PLATFORM}"
        ;;

    *)
        echo "Error: unsupported LLVM tool '${TOOL}'."
        exit 1
        ;;
esac
