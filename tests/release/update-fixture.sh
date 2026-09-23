#!/bin/sh

# Builds a genuine next-version CUP binary and a minimal authenticated server tree for self-update.
set -eu

next_test_version() {
    version=$1
    old_ifs=$IFS
    IFS=.
    # shellcheck disable=SC2086
    set -- $version
    IFS=$old_ifs
    [ "$#" -eq 3 ] || return 1
    major=$1 minor=$2 patch=$3
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
# shellcheck disable=SC2046
set -- $(release_public_assets)
validate_exact_directory_files "$candidate" "$@"
for asset in "$@"; do require_nonempty_file "$candidate/$asset"; done
validate_release_asset_modes "$candidate" "$@"
validate_release_file "$candidate/release.txt"

next_version=$(next_test_version "$VERSION") || fail "no supported semantic version follows $VERSION"
temporary_parent=$(cup_path_resolve_host_temporary_directory \
    'release update VERSION temporary directory') ||
    fail 'could not resolve update VERSION temporary directory'
version_file=$(mktemp "$temporary_parent/cup-release-update-version.XXXXXX") ||
    fail 'could not create update VERSION fixture'
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

cleanup_update_builder() { rm -f -- "$version_file"; }
trap cleanup_update_builder EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
printf '%s\n' "$next_version" > "$version_file" || fail 'could not write update VERSION fixture'

if [ -e "$fixture_build_root" ] || [ -L "$fixture_build_root" ]; then
    cup_path_require_build_root "$fixture_build_root" ||
        fail "previous update fixture build root is invalid: $fixture_build_root"
    cup_path_clean_build_root "$fixture_build_root" ||
        fail "could not clean previous update fixture build root: $fixture_build_root"
fi
cup_path_prepare_build_root "$fixture_build_root" ||
    fail "could not initialize update fixture build root: $fixture_build_root"
cup_path_prepare_child_directory "$fixture_build_root" "$fixture_common" 'update fixture common directory'

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

case "$PLATFORM" in
    windows-x64) binary_name=cup-windows-x64.exe ;;
    linux-x64|linux-arm64|macos-x64|macos-arm64) binary_name=cup-$PLATFORM ;;
    *) fail "unsupported release platform: $PLATFORM" ;;
esac
require_nonempty_file "$fixture_public/$binary_name"

prepare_output_staging "$fixture_root" "$BUILD_ROOT"
cleanup_fixture_staging() {
    [ -z "${OUTPUT_STAGING:-}" ] ||
        cup_path_remove_child_tree "$BUILD_ROOT" "$OUTPUT_STAGING" 'release test fixture staging'
}
cleanup_update_fixture() { cleanup_fixture_staging; cleanup_update_builder; }
trap cleanup_update_fixture EXIT
server_root=$OUTPUT_STAGING

# The root server still exposes the exact candidate for fresh-install tests.
# shellcheck disable=SC2046
set -- $(release_public_assets)
for asset in "$@"; do
    mode=$(release_asset_mode "$asset")
    cup_path_copy_file "$candidate/$asset" "$server_root/$asset" "$mode" replace ||
        fail "could not copy release test asset: $asset"
done

# Self-update discovery points at one concrete target manifest, then all authenticated
# generation assets are fetched from the versioned directory below.
update_root=$server_root/update-fixture
version_root=$update_root/$next_version
cup_path_prepare_child_directory "$BUILD_ROOT" "$version_root" 'versioned update fixture'
for asset in LICENSE THIRD_PARTY_NOTICES.txt; do
    cup_path_copy_file "$candidate/$asset" "$version_root/$asset" 0644 replace ||
        fail "could not copy update generation asset: $asset"
done
cup_path_copy_file "$fixture_public/$binary_name" "$version_root/$binary_name" \
    "$(release_asset_mode "$binary_name")" replace || fail 'could not copy genuine update executable'
VERSION=$next_version SHA=$SHA generate_release_file "$version_root"
validate_exact_directory_files "$version_root" \
    LICENSE THIRD_PARTY_NOTICES.txt "$binary_name" release.txt
VERSION=$next_version SHA=$SHA validate_release_file "$version_root/release.txt"
cup_path_copy_file "$version_root/release.txt" "$update_root/release.txt" 0644 replace ||
    fail 'could not publish update discovery manifest'

commit_output_staging "$fixture_root"
OUTPUT_STAGING=
cleanup_update_builder
trap - EXIT HUP INT TERM
printf '%s\n' "$fixture_root"
