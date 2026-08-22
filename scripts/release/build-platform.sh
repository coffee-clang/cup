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
case "$PLATFORM" in
    linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
    *) fail "unsupported release platform: $PLATFORM" ;;
esac

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
validate_release_file "$COMMON/release.txt"
require_nonempty_file "$COMMON/SHA256SUMS.common"
require_nonempty_file "$FINALIZED/build-config.txt"
require_nonempty_file "$FINALIZED/release.txt"
require_nonempty_file "$FINALIZED/binary-inspection.txt"
require_nonempty_file "$FINALIZED/finalization.txt"
cmp -s "$COMMON/release.txt" "$FINALIZED/release.txt" ||
    fail 'finalized build metadata differs from common release metadata'

if [ "$PLATFORM" = windows-x64 ]; then
    source_binary=$FINALIZED/bin/cup.exe
    public_binary=cup-$PLATFORM.exe
else
    source_binary=$FINALIZED/bin/cup
    public_binary=cup-$PLATFORM
fi
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
if [ "$PLATFORM" = windows-x64 ]; then public_mode=0644; else public_mode=0755; fi
cup_path_copy_file "$source_binary" "$PUBLIC/$public_binary" "$public_mode" replace ||
    fail "could not copy public release binary"

{
    printf '%s  %s\n' "$(hash_file "$PUBLIC/$public_binary")" "$public_binary"
    printf '%s  release.txt\n' "$(hash_file "$COMMON/release.txt")"
    printf '%s  SHA256SUMS.common\n' "$(hash_file "$COMMON/SHA256SUMS.common")"
} | cup_path_write_file "$PUBLIC/SHA256SUMS.$PLATFORM" 0644 replace

cup_path_copy_tree "$FINALIZED/symbols" "$SYMBOLS" ||
    fail "could not copy finalized symbols"
for metadata in build-config.txt release.txt binary-inspection.txt finalization.txt; do
    cup_path_copy_file "$FINALIZED/$metadata" "$SYMBOLS/$metadata" 0644 replace ||
        fail "could not copy finalized metadata: $metadata"
done

# Verify the checksum against an exact temporary assembled view containing the
# common files referenced by the platform checksum.
VERIFY=$OUTPUT_STAGING/.verify
cup_path_prepare_child_directory "$BUILD_ROOT" "$VERIFY" "checksum verification directory"
cup_path_copy_file "$PUBLIC/$public_binary" "$VERIFY/$public_binary" "$public_mode" replace
cup_path_copy_file "$PUBLIC/SHA256SUMS.$PLATFORM" "$VERIFY/SHA256SUMS.$PLATFORM" 0644 replace
cup_path_copy_file "$COMMON/release.txt" "$VERIFY/release.txt" 0644 replace
cup_path_copy_file "$COMMON/SHA256SUMS.common" "$VERIFY/SHA256SUMS.common" 0644 replace
verify_checksum_file_exact "$VERIFY" "SHA256SUMS.$PLATFORM" \
    "$public_binary" release.txt SHA256SUMS.common
cup_path_remove_child_tree "$BUILD_ROOT" "$VERIFY" 'checksum verification directory'

validate_exact_directory_files "$PUBLIC" "$public_binary" "SHA256SUMS.$PLATFORM"
commit_output_staging "$OUTPUT"
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
