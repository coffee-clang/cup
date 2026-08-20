# Provides the shared release-script library for exact asset validation and hashing.
# This file is sourced by release assembly and publication entry points.
set -eu

: "${SCRIPT_DIR:?release SCRIPT_DIR is required before sourcing common.sh}"
# shellcheck source=../lib/path-safety.sh
. "$SCRIPT_DIR/../lib/path-safety.sh"

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
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$hash_input" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$hash_input" | awk '{print $1}'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
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

validate_repository_identifier() {
    repository_identifier=$1
    repository_label=${2:-repository}
    printf '%s\n' "$repository_identifier" |
        grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' ||
        fail "invalid $repository_label: $repository_identifier"
    repository_owner=${repository_identifier%%/*}
    repository_name=${repository_identifier#*/}
    case "$repository_owner:$repository_name" in
        .:*|..:*|*:.|*:..)
            fail "invalid $repository_label: $repository_identifier"
            ;;
    esac
}

validate_release_inputs() {
    : "${VERSION:?VERSION is required}"
    : "${TAG:?TAG is required}"
    : "${SHA:?SHA is required}"
    [ "$TAG" = "v$VERSION" ] || fail 'TAG does not match VERSION'
    printf '%s\n' "$VERSION" |
        grep -Eq '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$' ||
        fail "invalid VERSION: $VERSION"
    printf '%s\n' "$SHA" | grep -Eq '^[0-9a-f]{40}$' || fail "invalid SHA: $SHA"
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

verify_checksums() {
    checksum_directory=$1
    shift
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$checksum_directory" && sha256sum -c "$@")
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$checksum_directory" && shasum -a 256 -c "$@")
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

verify_checksum_file_exact() {
    checksum_directory=$1
    checksum_file=$2
    shift 2
    checksum_path=$checksum_directory/$checksum_file
    require_nonempty_file "$checksum_path"

    entry_count=$(awk 'NF > 0 { count++ } END { print count + 0 }' "$checksum_path")
    [ "$entry_count" -eq "$#" ] ||
        fail "checksum file $checksum_file has $entry_count entries; expected $#"

    for expected_asset in "$@"; do
        require_nonempty_file "$checksum_directory/$expected_asset"
        checksum_matches=$(awk -v name="$expected_asset" '
            /^[0-9a-f]{64}[[:space:]]+\*?[^[:space:]].*$/ {
                file=$0
                sub(/^[0-9a-f]{64}[[:space:]]+\*?/, "", file)
                if (file == name) count++
            }
            END { print count + 0 }
        ' "$checksum_path")
        [ "$checksum_matches" -eq 1 ] ||
            fail "checksum entry is missing or duplicated in $checksum_file: $expected_asset"
    done

    awk '
        /^[[:space:]]*$/ { exit 2 }
        !/^[0-9a-f]{64}[[:space:]]+\*?[^[:space:]].*$/ { exit 3 }
        {
            file=$0
            sub(/^[0-9a-f]{64}[[:space:]]+\*?/, "", file)
            if (file ~ /(^|\/)\.\.($|\/)/ || file ~ /^\// || file ~ /^[A-Za-z]:/ || file ~ /\\/) exit 4
        }
    ' "$checksum_path" || fail "checksum file contains invalid or unsafe entries: $checksum_file"
    verify_checksums "$checksum_directory" "$checksum_file"
}

validate_release_file() {
    release_file=$1
    require_nonempty_file "$release_file"
    awk -F= -v version="$VERSION" -v sha="$SHA" '
        $1 == "format" && NF == 2 && $2 == "1" { f++; next }
        $1 == "version" && NF == 2 && $2 == version { v++; next }
        $1 == "commit" && NF == 2 && $2 == sha { c++; next }
        { invalid=1 }
        END { if (invalid || NR != 3 || f != 1 || v != 1 || c != 1) exit 1 }
    ' "$release_file" || fail "invalid release metadata: $release_file"
}

release_asset_mode() {
    case "$1" in
        install.sh|uninstall.sh|cup-linux-x64|cup-linux-arm64|cup-macos-x64|cup-macos-arm64)
            printf '0755\n'
            ;;
        *)
            printf '0644\n'
            ;;
    esac
}

validate_release_asset_modes() {
    asset_directory=$1
    shift
    for asset_name in "$@"; do
        expected_mode=$(release_asset_mode "$asset_name")
        actual_mode=$(stat -c '%a' "$asset_directory/$asset_name" 2>/dev/null ||
            stat -f '%Lp' "$asset_directory/$asset_name" 2>/dev/null) ||
            fail "could not inspect release asset mode: $asset_name"
        [ "$actual_mode" = "${expected_mode#0}" ] ||
            fail "release asset has mode $actual_mode, expected ${expected_mode#0}: $asset_name"
    done
}

validate_provenance_file() {
    provenance_file=$1
    expected_repository=${2:-}
    expected_tests_run_id=${3:-}
    expected_tests_run_attempt=${4:-}
    expected_tests_index_sha256=${5:-}
    expected_release_run_id=${6:-}
    expected_release_run_attempt=${7:-}
    require_nonempty_file "$provenance_file"
    awk -F= \
        -v version="$VERSION" -v sha="$SHA" \
        -v expected_repository="$expected_repository" \
        -v expected_tests_run_id="$expected_tests_run_id" \
        -v expected_tests_run_attempt="$expected_tests_run_attempt" \
        -v expected_tests_index_sha256="$expected_tests_index_sha256" \
        -v expected_release_run_id="$expected_release_run_id" \
        -v expected_release_run_attempt="$expected_release_run_attempt" '
        function valid_repository(value) { return value ~ /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/ }
        function valid_number(value) { return value ~ /^[1-9][0-9]*$/ }
        function valid_sha256(value) { return value ~ /^[0-9a-f]{64}$/ }
        $1 == "format" && NF == 2 && $2 == "3" { f++; next }
        $1 == "version" && NF == 2 && $2 == version { v++; next }
        $1 == "source_repository" && NF == 2 && valid_repository($2) { repo=$2; r++; next }
        $1 == "source_commit" && NF == 2 && $2 == sha { c++; next }
        $1 == "tests_run_id" && NF == 2 && valid_number($2) { tests=$2; t++; next }
        $1 == "tests_run_attempt" && NF == 2 && valid_number($2) { tests_attempt=$2; a++; next }
        $1 == "tests_evidence_index_sha256" && NF == 2 && valid_sha256($2) { index_sha=$2; i++; next }
        $1 == "release_run_id" && NF == 2 && valid_number($2) { release=$2; q++; next }
        $1 == "release_run_attempt" && NF == 2 && valid_number($2) { release_attempt=$2; p++; next }
        { invalid=1 }
        END {
            if (invalid || NR != 9 || f != 1 || v != 1 || r != 1 || c != 1 ||
                    t != 1 || a != 1 || i != 1 || q != 1 || p != 1) exit 1
            if (expected_repository != "" && repo != expected_repository) exit 1
            if (expected_tests_run_id != "" && tests != expected_tests_run_id) exit 1
            if (expected_tests_run_attempt != "" && tests_attempt != expected_tests_run_attempt) exit 1
            if (expected_tests_index_sha256 != "" && index_sha != expected_tests_index_sha256) exit 1
            if (expected_release_run_id != "" && release != expected_release_run_id) exit 1
            if (expected_release_run_attempt != "" && release_attempt != expected_release_run_attempt) exit 1
        }
    ' "$provenance_file" || fail "invalid provenance file: $provenance_file"
}
