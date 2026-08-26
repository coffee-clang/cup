#!/usr/bin/env sh

# Builds a genuine newer official cup executable and one private HTTP fixture tree.
set -eu

next_test_version() {
    old=$1
    case "$old" in
        ''|*[!0-9.]*|.*|*..*|*.) return 1 ;;
    esac
    old_ifs=$IFS
    IFS=.
    set -- $old
    IFS=$old_ifs
    [ "$#" -eq 3 ] || return 1
    major=$1
    minor=$2
    patch=$3
    for part in "$major" "$minor" "$patch"; do
        case "$part" in ''|*[!0-9]*) return 1 ;; esac
        case "$part" in 0) ;; 0*) return 1 ;; esac
        [ "${#part}" -le 6 ] && [ "$part" -le 999999 ] || return 1
    done
    if [ "$patch" -lt 999999 ]; then
        printf '%s.%s.%s\n' "$major" "$minor" "$((patch + 1))"
    elif [ "$minor" -lt 999999 ]; then
        printf '%s.%s.0\n' "$major" "$((minor + 1))"
    elif [ "$major" -lt 999999 ]; then
        printf '%s.0.0\n' "$((major + 1))"
    else
        return 1
    fi
}

if [ "${1:-}" = --next-version ]; then
    [ "$#" -eq 2 ] || { printf 'Usage: %s --next-version <version>\n' "$0" >&2; exit 2; }
    next_test_version "$2" || {
        printf 'No supported semantic version follows %s.\n' "$2" >&2
        exit 1
    }
    exit 0
fi

[ "$#" -eq 2 ] || {
    printf 'Usage: %s <candidate-dir> <fixture-root>\n' "$0" >&2
    exit 2
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
SCRIPT_DIR=$ROOT/scripts/release
. "$SCRIPT_DIR/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${VERSION:?VERSION is required}"
: "${SHA:?SHA is required}"
: "${DEPS_ROOT:?DEPS_ROOT is required}"
: "${DEPS_PREFIX:?DEPS_PREFIX is required}"
: "${CC:?CC is required}"
: "${CUP_BUILD_DIR:?CUP_BUILD_DIR is required}"
BUILD_ROOT=${CUP_BUILD_ROOT:-$ROOT/build}
candidate=$1
fixture_root=$2
case "$candidate" in /*) ;; *) candidate=$ROOT/$candidate ;; esac
case "$fixture_root" in /*) ;; *) fixture_root=$ROOT/$fixture_root ;; esac

validate_build_root "$BUILD_ROOT"
require_real_directory "$candidate"
cup_path_require_within "$BUILD_ROOT" "$fixture_root" 'release test fixture' ||
    fail 'release test fixture must stay inside the managed build root'

# Reject malformed candidates before spending time on the genuine next-version build.
set -- packages.cfg install.cfg release.txt provenance.txt THIRD_PARTY_NOTICES.txt \
    install.sh install.ps1 SHA256SUMS.common "SHA256SUMS.$PLATFORM"
case "$PLATFORM" in
    windows-x64) binary_name=cup-windows-x64.exe ;;
    linux-x64|linux-arm64|macos-x64|macos-arm64) binary_name=cup-$PLATFORM ;;
    *) fail "unsupported release platform: $PLATFORM" ;;
esac
set -- "$@" "$binary_name"
validate_exact_directory_files "$candidate" "$@"
for asset in "$@"; do require_nonempty_file "$candidate/$asset"; done
validate_release_asset_modes "$candidate" "$@"
verify_checksum_file_exact "$candidate" SHA256SUMS.common \
    packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$candidate" "SHA256SUMS.$PLATFORM" \
    "$binary_name" release.txt SHA256SUMS.common
validate_release_file "$candidate/release.txt"

next_version=$(next_test_version "$VERSION") ||
    fail "no supported semantic version follows $VERSION"
# Keep the test-only VERSION outside the checkout so an official fixture build remains clean even
# when BUILD_DIR names a custom, non-ignored directory inside the repository.
temporary_parent=$(cup_path_resolve_host_temporary_directory \
    'release update VERSION temporary directory') ||
    fail 'could not resolve update VERSION temporary directory'
version_file=$(mktemp "$temporary_parent/cup-release-update-version.XXXXXX") ||
    fail 'could not create update VERSION fixture'
# Keep the recursive Make selector relative when the parent BUILD_DIR is relative; converting it
# to an absolute checkout path would incorrectly reject otherwise-supported checkout spaces.
fixture_build_dir=$CUP_BUILD_DIR/release-test-update-build-$PLATFORM
fixture_build_root=$BUILD_ROOT/release-test-update-build-$PLATFORM
case "$CUP_BUILD_DIR" in
    /*) expected_fixture_build_root=$CUP_BUILD_DIR/release-test-update-build-$PLATFORM ;;
    *) expected_fixture_build_root=$ROOT/$CUP_BUILD_DIR/release-test-update-build-$PLATFORM ;;
esac
[ "$fixture_build_root" = "$expected_fixture_build_root" ] ||
    fail 'CUP_BUILD_DIR and CUP_BUILD_ROOT do not describe the same managed build root'
fixture_common_dir=$fixture_build_dir/release-test-common
fixture_common=$fixture_build_root/release-test-common
fixture_public=$fixture_build_root/release/platforms/$PLATFORM/public

cleanup_update_builder() {
    rm -f -- "$version_file"
}
trap cleanup_update_builder EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
printf '%s\n' "$next_version" > "$version_file" ||
    fail 'could not write update VERSION fixture'

if [ -e "$fixture_build_root" ] || [ -L "$fixture_build_root" ]; then
    cup_path_require_build_root "$fixture_build_root" ||
        fail "previous update fixture build root is invalid: $fixture_build_root"
    cup_path_clean_build_root "$fixture_build_root" ||
        fail "could not clean previous update fixture build root: $fixture_build_root"
fi
cup_path_prepare_build_root "$fixture_build_root" ||
    fail "could not initialize update fixture build root: $fixture_build_root"
cup_path_prepare_child_directory "$fixture_build_root" "$fixture_common" 'update fixture common metadata'
# The public platform producer needs only release identity and the authenticated common checksum.
# The updater-specific server tree below supplies the unchanged common assets it actually consumes.
printf 'format=1\nversion=%s\ncommit=%s\n' "$next_version" "$SHA" |
    cup_path_write_file "$fixture_common/release.txt" 0644 replace ||
    fail 'could not create update release metadata'
cup_path_copy_file "$candidate/SHA256SUMS.common" "$fixture_common/SHA256SUMS.common" 0644 replace ||
    fail 'could not copy update common checksums'
(
    VERSION=$next_version
    validate_release_file "$fixture_common/release.txt"
)

set -- make -C "$ROOT" --no-print-directory release-candidate \
    "PLATFORM=$PLATFORM" "BUILD_DIR=$fixture_build_dir" \
    "DEPS_ROOT=$DEPS_ROOT" "DEPS_PREFIX=$DEPS_PREFIX" "CC=$CC" \
    "RELEASE_COMMON_DIR=$fixture_common_dir" \
    "CUP_RELEASE_VERSION=$next_version" "CUP_RELEASE_TAG=v$next_version" \
    "CUP_RELEASE_COMMIT=$SHA"
if [ "$PLATFORM" = windows-x64 ]; then
    : "${WINDRES:?WINDRES is required for windows-x64}"
    set -- "$@" "WINDRES=$WINDRES"
fi
CUP_VERSION_FILE=$version_file "$@"

require_nonempty_file "$fixture_public/$binary_name"
require_nonempty_file "$fixture_public/SHA256SUMS.$PLATFORM"

prepare_output_staging "$fixture_root" "$BUILD_ROOT"
cleanup_fixture_staging() {
    [ -z "${OUTPUT_STAGING:-}" ] ||
        cup_path_remove_child_tree "$BUILD_ROOT" "$OUTPUT_STAGING" 'release test fixture staging'
}
cleanup_update_fixture() {
    cleanup_fixture_staging
    cleanup_update_builder
}
trap cleanup_update_fixture EXIT
server_root=$OUTPUT_STAGING

set -- packages.cfg install.cfg release.txt provenance.txt THIRD_PARTY_NOTICES.txt \
    install.sh install.ps1 SHA256SUMS.common "SHA256SUMS.$PLATFORM" "$binary_name"
for asset in "$@"; do
    mode=$(release_asset_mode "$asset")
    cup_path_copy_file "$candidate/$asset" "$server_root/$asset" "$mode" replace ||
        fail "could not copy release test asset: $asset"
done

update_root=$server_root/update-fixture
version_root=$update_root/$next_version
cup_path_prepare_child_directory "$BUILD_ROOT" "$version_root" 'versioned update fixture'
for asset in packages.cfg install.cfg install.sh install.ps1 SHA256SUMS.common; do
    mode=$(release_asset_mode "$asset")
    cup_path_copy_file "$candidate/$asset" "$version_root/$asset" "$mode" replace ||
        fail "could not copy versioned update asset: $asset"
done
cup_path_copy_file "$fixture_public/$binary_name" "$version_root/$binary_name" \
    "$(release_asset_mode "$binary_name")" replace ||
    fail 'could not copy genuine update executable'
cup_path_copy_file "$fixture_common/release.txt" "$version_root/release.txt" 0644 replace ||
    fail 'could not copy genuine update metadata'
cup_path_copy_file "$fixture_common/release.txt" "$update_root/release.txt" 0644 replace ||
    fail 'could not copy latest update metadata'
cup_path_copy_file "$fixture_public/SHA256SUMS.$PLATFORM" \
    "$version_root/SHA256SUMS.$PLATFORM" 0644 replace ||
    fail 'could not copy genuine platform checksums'

set -- packages.cfg install.cfg install.sh install.ps1 SHA256SUMS.common \
    "SHA256SUMS.$PLATFORM" "$binary_name" release.txt
validate_exact_directory_files "$version_root" "$@"
verify_checksum_file_exact "$version_root" SHA256SUMS.common \
    packages.cfg install.cfg install.sh install.ps1
(
    VERSION=$next_version
    validate_release_file "$version_root/release.txt"
)
verify_checksum_file_exact "$version_root" "SHA256SUMS.$PLATFORM" \
    "$binary_name" release.txt SHA256SUMS.common

commit_output_staging "$fixture_root"
OUTPUT_STAGING=
cleanup_update_builder
trap - EXIT HUP INT TERM
printf '%s\n' "$fixture_root"
