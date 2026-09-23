#!/bin/sh

# Assembles one already-finalized native candidate with exact checksums.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
. "$SCRIPT_DIR/common.sh"

validate_release_inputs
: "${PLATFORM:?PLATFORM is required}"
cup_platform_valid "$PLATFORM" || fail "unsupported release platform: $PLATFORM"

[ "$#" -eq 3 ] || {
    printf 'Usage: %s <common-public-dir> <finalized-dir> <output-dir>\n' "$0" >&2
    exit 2
}
COMMON=$1
FINALIZED=$2
OUTPUT=$3
case "$COMMON" in /*) ;; *) COMMON=$PROJECT_ROOT/$COMMON ;; esac
case "$FINALIZED" in /*) ;; *) FINALIZED=$PROJECT_ROOT/$FINALIZED ;; esac
case "$OUTPUT" in /*) ;; *) OUTPUT=$PROJECT_ROOT/$OUTPUT ;; esac
BUILD_ROOT=${CUP_BUILD_ROOT:-$PROJECT_ROOT/build}

require_real_directory "$COMMON"
require_real_directory "$FINALIZED"
require_nonempty_file "$FINALIZED/build-config.txt"
require_nonempty_file "$FINALIZED/binary-inspection.txt"
require_nonempty_file "$FINALIZED/finalization.txt"

if [ "$PLATFORM" = windows-x64 ]; then
    source_binary=$FINALIZED/bin/cup.exe
else
    source_binary=$FINALIZED/bin/cup
fi
public_binary=$(release_platform_binary_name "$PLATFORM") ||
    fail "could not derive public binary name: $PLATFORM"
require_nonempty_file "$source_binary"
require_real_directory "$FINALIZED/symbols"
cup_path_require_safe_tree "$FINALIZED/symbols" "finalized symbols" ||
    fail "finalized symbols contain an unsafe entry"
case "$PLATFORM" in
    linux-*|windows-x64) validate_exact_directory_files "$FINALIZED/symbols" cup.debug ;;
    macos-*) validate_exact_directory_files "$FINALIZED/symbols" cup.dSYM ;;
esac

prepare_output_staging "$OUTPUT" "$BUILD_ROOT"
cleanup_platform() {
    [ -z "${OUTPUT_STAGING:-}" ] || \
        cup_path_remove_child_tree \
            "$BUILD_ROOT" "$OUTPUT_STAGING" 'platform release staging'
}
trap cleanup_platform EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
PUBLIC=$OUTPUT_STAGING/public
SYMBOLS=$OUTPUT_STAGING/symbols
cup_path_prepare_child_directory "$BUILD_ROOT" "$PUBLIC" "platform public directory"
cup_path_prepare_child_directory "$BUILD_ROOT" "$SYMBOLS" "platform symbols directory"
public_mode=$(release_platform_binary_mode "$PLATFORM") ||
    fail "could not derive public binary mode: $PLATFORM"
cup_path_copy_file "$source_binary" "$PUBLIC/$public_binary" "$public_mode" replace ||
    fail "could not copy public release binary"

cup_path_copy_tree "$FINALIZED/symbols" "$SYMBOLS" ||
    fail "could not copy finalized symbols"
for metadata in build-config.txt binary-inspection.txt finalization.txt; do
    cup_path_copy_file "$FINALIZED/$metadata" "$SYMBOLS/$metadata" 0644 replace ||
        fail "could not copy finalized metadata: $metadata"
done

validate_exact_directory_files "$PUBLIC" "$public_binary"
commit_output_staging "$OUTPUT"
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
