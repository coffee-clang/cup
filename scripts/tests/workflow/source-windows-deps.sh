#!/usr/bin/env bash
set -euo pipefail

prefix=${1:-/ucrt64}
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_root="$(CDPATH= cd -- "$script_dir/../../.." && pwd)"
source "$project_root/scripts/bootstrap/bootstrap-common.sh"

src_dir="${RUNNER_TEMP:-/tmp}/cup-windows-source-deps/src"
build_dir="${RUNNER_TEMP:-/tmp}/cup-windows-source-deps/build"
mkdir -p "$src_dir" "$build_dir" "$prefix/include" "$prefix/lib"

archive="$src_dir/uthash-${UTHASH_VERSION}.tar.gz"
source_dir="$build_dir/uthash-${UTHASH_VERSION}"
download_source uthash "$archive"
extract_archive "$archive" "$source_dir"
cp "$source_dir/src/uthash.h" "$prefix/include/uthash.h"

archive="$src_dir/unity-${UNITY_VERSION}.tar.gz"
source_dir="$build_dir/Unity-${UNITY_VERSION}"
download_source unity "$archive"
extract_archive "$archive" "$source_dir"
cp "$source_dir/src/unity.h" "$source_dir/src/unity_internals.h" "$prefix/include/"
gcc -std=c11 -O2 -I"$source_dir/src" -c "$source_dir/src/unity.c" \
    -o "$build_dir/unity.o"
ar rcs "$prefix/lib/libunity.a" "$build_dir/unity.o"
