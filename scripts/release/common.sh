# Provides the shared release-script library for exact asset validation and hashing.
# This file is sourced by release assembly and publication entry points.
set -eu

: "${SCRIPT_DIR:?release SCRIPT_DIR is required before sourcing common.sh}"
# shellcheck source=../lib/path-safety.sh
. "$SCRIPT_DIR/../lib/path-safety.sh"
# shellcheck source=../lib/sha256.sh
. "$SCRIPT_DIR/../lib/sha256.sh"
# shellcheck source=../lib/repository-id.sh
. "$SCRIPT_DIR/../lib/repository-id.sh"
# shellcheck source=../lib/git-identity.sh
. "$SCRIPT_DIR/../lib/git-identity.sh"
# shellcheck source=../lib/platform-domain.sh
. "$SCRIPT_DIR/../lib/platform-domain.sh"
# shellcheck source=../lib/semver.sh
. "$SCRIPT_DIR/../lib/semver.sh"

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '%s\n' "$*"
}

hash_file() {
    hash_input=$1
    require_regular_file "$hash_input"
    cup_sha256_file "$hash_input" ||
        fail 'neither sha256sum nor shasum produced a valid SHA-256 digest'
}

require_regular_file() {
    regular_path=$1
    cup_path_require_regular_file "$regular_path" "release file" ||
        fail "expected a regular no-follow file: $regular_path"
}

require_nonempty_file() {
    nonempty_path=$1
    require_regular_file "$nonempty_path"
    [ -s "$nonempty_path" ] || fail "expected a non-empty file: $nonempty_path"
}

require_real_directory() {
    real_directory=$1
    cup_path_check_directory_chain "$real_directory" 0 "release directory" ||
        fail "expected a real no-follow directory: $real_directory"
}

validate_repository_identifier() (
    repository_identifier=$1
    repository_label=${2:-repository}
    cup_repository_identifier_valid "$repository_identifier" ||
        fail "invalid $repository_label: $repository_identifier"
)

validate_release_inputs() {
    : "${VERSION:?VERSION is required}"
    : "${TAG:?TAG is required}"
    : "${SHA:?SHA is required}"
    [ "$TAG" = "v$VERSION" ] || fail 'TAG does not match VERSION'
    cup_semver_valid "$VERSION" || fail "invalid VERSION: $VERSION"
    cup_git_commit_valid "$SHA" || fail "invalid SHA: $SHA"
}

validate_build_root() {
    release_build_root=$1
    cup_path_require_build_root "$release_build_root" ||
        fail "invalid build root: $release_build_root"
}

prepare_output_staging() {
    output=$1
    build_root=$2
    validate_build_root "$build_root"
    OUTPUT_BUILD_ROOT=$build_root
    cup_path_require_within "$build_root" "$output" "release output" ||
        fail "output must be inside the managed build root: $output"
    output_parent=$(dirname -- "$output")
    cup_path_prepare_child_directory "$build_root" "$output_parent" \
        "release output parent" || exit 1
    OUTPUT_STAGING=$(cup_path_create_unique_directory \
        "$output_parent/.release-output.XXXXXX" "release output staging" 0755) || exit 1
    cup_path_check_directory_chain "$OUTPUT_STAGING" 0 \
        "release output staging" || exit 1
}

commit_output_staging() {
    output=$1
    [ -n "${OUTPUT_STAGING:-}" ] || fail 'release output staging was not prepared'
    require_real_directory "$OUTPUT_STAGING"
    if [ -e "$output" ] || [ -L "$output" ]; then
        require_real_directory "$output"
        cup_path_remove_child_tree "$OUTPUT_BUILD_ROOT" "$output" 'existing release output' ||
            fail "could not remove previous release output: $output"
    fi
    cup_path_move_entry "$OUTPUT_STAGING" "$output" ||
        fail "could not commit release output: $output"
    OUTPUT_STAGING=
    require_real_directory "$output"
    OUTPUT_BUILD_ROOT=
}

validate_exact_directory_files() (
    exact_directory=$1
    shift
    require_real_directory "$exact_directory"
    expected_file=$(mktemp "${TMPDIR:-/tmp}/cup-release-expected.XXXXXX")
    actual_file=$(mktemp "${TMPDIR:-/tmp}/cup-release-actual.XXXXXX")
    cleanup_exact_files() { rm -f -- "$expected_file" "$actual_file"; }
    trap cleanup_exact_files EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    printf '%s\n' "$@" | LC_ALL=C sort > "$expected_file"
    (
        cd "$exact_directory"
        find . -mindepth 1 -maxdepth 1 -print | sed 's|^\./||' | LC_ALL=C sort
    ) > "$actual_file"
    if ! cmp -s "$expected_file" "$actual_file"; then
        printf 'Expected entries in %s:\n' "$exact_directory" >&2
        cat "$expected_file" >&2
        printf 'Actual entries in %s:\n' "$exact_directory" >&2
        cat "$actual_file" >&2
        fail "unexpected release output entries in $exact_directory"
    fi
)

prepare_installer() {
    installer_source=$1
    installer_destination=$2
    installer_mode=${3:-0644}
    require_nonempty_file "$installer_source"
    sed \
        -e "s|@CUP_RELEASE_VERSION@|$VERSION|g" \
        -e "s|@CUP_RELEASE_TAG@|$TAG|g" \
        -e "s|@CUP_RELEASE_COMMIT@|$SHA|g" \
        "$installer_source" | cup_path_write_file "$installer_destination" "$installer_mode" replace ||
        fail "could not prepare installer: $installer_destination"
}

verify_checksum_file_exact() (
    checksum_directory=$1
    checksum_file=$2
    shift 2
    checksum_path=$checksum_directory/$checksum_file
    require_nonempty_file "$checksum_path"

    expected_checksums=$(mktemp "${TMPDIR:-/tmp}/cup-release-checksums.XXXXXX") ||
        fail 'could not create checksum comparison file'
    cleanup_expected_checksums() { rm -f -- "$expected_checksums"; }
    trap cleanup_expected_checksums EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    for expected_asset in "$@"; do
        require_nonempty_file "$checksum_directory/$expected_asset"
        printf '%s  %s\n' "$(hash_file "$checksum_directory/$expected_asset")" "$expected_asset"
    done > "$expected_checksums"
    cmp -s "$expected_checksums" "$checksum_path" ||
        fail "checksum file is not the exact canonical document: $checksum_file"
    rm -f -- "$expected_checksums"
    trap - EXIT HUP INT TERM
)

validate_release_file() (
    release_file=$1
    require_nonempty_file "$release_file"
    expected_release=$(mktemp "${TMPDIR:-/tmp}/cup-release-metadata.XXXXXX") ||
        fail 'could not create release metadata comparison file'
    cleanup_expected_release() { rm -f -- "$expected_release"; }
    trap cleanup_expected_release EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    printf 'format=1\nversion=%s\ncommit=%s\n' "$VERSION" "$SHA" > "$expected_release"
    cmp -s "$expected_release" "$release_file" ||
        fail "invalid release metadata: $release_file"
    rm -f -- "$expected_release"
    trap - EXIT HUP INT TERM
)

release_common_checksum_assets() (
    printf '%s\n' packages.cfg install.cfg install.sh install.ps1
)

release_common_public_assets() (
    printf '%s\n' \
        THIRD_PARTY_NOTICES.txt SHA256SUMS.common install.cfg install.ps1 install.sh \
        packages.cfg provenance.txt release.txt
)

release_platform_binary_name() (
    platform=$1
    cup_platform_valid "$platform" || return 1
    case "$platform" in
        windows-x64) printf 'cup-%s.exe\n' "$platform" ;;
        *) printf 'cup-%s\n' "$platform" ;;
    esac
)

release_platform_binary_mode() (
    platform=$1
    cup_platform_valid "$platform" || return 1
    case "$platform" in
        windows-x64) printf '0644\n' ;;
        *) printf '0755\n' ;;
    esac
)

release_platform_checksum_name() (
    platform=$1
    cup_platform_valid "$platform" || return 1
    printf 'SHA256SUMS.%s\n' "$platform"
)

release_platform_checksum_assets() (
    platform=$1
    binary=$(release_platform_binary_name "$platform") || return 1
    printf '%s\n' "$binary" release.txt SHA256SUMS.common
)

release_public_assets() (
    printf '%s\n' packages.cfg install.cfg release.txt provenance.txt \
        THIRD_PARTY_NOTICES.txt install.sh install.ps1
    for platform in $CUP_SUPPORTED_PLATFORMS; do
        release_platform_binary_name "$platform" || return 1
    done
    printf '%s\n' SHA256SUMS.common
    for platform in $CUP_SUPPORTED_PLATFORMS; do
        release_platform_checksum_name "$platform" || return 1
    done
)

release_asset_mode() (
    asset=$1
    [ "$asset" = install.sh ] && { printf '0755\n'; return 0; }
    for platform in $CUP_SUPPORTED_PLATFORMS; do
        binary=$(release_platform_binary_name "$platform") || return 1
        if [ "$asset" = "$binary" ]; then
            release_platform_binary_mode "$platform"
            return
        fi
    done
    printf '0644\n'
)

validate_release_asset_modes() {
    asset_directory=$1
    shift
    for asset_name in "$@"; do
        expected_mode=$(release_asset_mode "$asset_name")
        actual_mode=$(stat -c '%a' "$asset_directory/$asset_name" 2>/dev/null ||
            stat -f '%Lp' "$asset_directory/$asset_name" 2>/dev/null) ||
            fail "could not inspect release asset mode: $asset_name"
        if [ "$actual_mode" != "${expected_mode#0}" ]; then
            # MSYS2 synthesizes the executable bit for .exe files from the filename rather
            # than a portable Unix mode. The final release snapshot is assembled on POSIX and
            # still normalizes the Windows executable to the canonical 0644 mode there.
            case "${MSYSTEM:-}" in
                UCRT64|CLANG64)
                    windows_binary=$(release_platform_binary_name windows-x64) ||
                        fail 'could not derive Windows release binary name'
                    [ "$asset_name" = "$windows_binary" ] &&
                        [ "$expected_mode" = 0644 ] && [ "$actual_mode" = 755 ] && continue
                    ;;
            esac
            fail "release asset has mode $actual_mode, expected ${expected_mode#0}: $asset_name"
        fi
    done
}

validate_release_provenance_inputs() (
    expected_repository=$1
    expected_tests_run_id=$2
    expected_tests_run_attempt=$3
    expected_release_run_id=$4

    validate_repository_identifier "$expected_repository" SOURCE_REPOSITORY
    for run_value in "$expected_tests_run_id" "$expected_tests_run_attempt" \
            "$expected_release_run_id"; do
        printf '%s\n' "$run_value" | grep -Eq '^[1-9][0-9]*$' ||
            fail 'invalid release provenance run identity'
    done
)

validate_provenance_file() {
    provenance_file=$1
    expected_repository=$2
    expected_tests_run_id=$3
    expected_tests_run_attempt=$4
    expected_release_run_id=$5
    require_nonempty_file "$provenance_file"
    {
        printf 'format=4\n'
        printf 'version=%s\n' "$VERSION"
        printf 'source_repository=%s\n' "$expected_repository"
        printf 'source_commit=%s\n' "$SHA"
        printf 'tests_run_id=%s\n' "$expected_tests_run_id"
        printf 'tests_run_attempt=%s\n' "$expected_tests_run_attempt"
        printf 'release_run_id=%s\n' "$expected_release_run_id"
    } | cmp -s - "$provenance_file" || fail "invalid provenance file: $provenance_file"
}
