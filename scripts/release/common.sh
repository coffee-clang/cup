#!/usr/bin/env sh

# Purpose: Shared release-script library for validation, hashing and immutable asset preparation.
# Sourced by candidate decision, assembly and publication entry points.
set -eu

# Basic diagnostics and hashing shared by every release entry point.
fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '%s\n' "$*"
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

# Validate the identity that all candidate artifacts must share.
validate_release_inputs() {
    : "${VERSION:?VERSION is required}"
    : "${TAG:?TAG is required}"
    : "${SHA:?SHA is required}"
    [ "$TAG" = "v$VERSION" ] || fail "TAG does not match VERSION"
    printf '%s\n' "$VERSION" |
        grep -Eq '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$' ||
        fail "invalid VERSION: $VERSION"
    printf '%s\n' "$SHA" | grep -Eq '^[0-9a-f]{40}$' ||
        fail "invalid SHA: $SHA"
}

# Validate the immutable provenance record shared by candidate inspection and
# publication. Empty expected values skip the corresponding workflow identity
# check while preserving the file format and source identity checks.
validate_provenance_file() {
    file=$1
    expected_repository=${2:-}
    expected_run_id=${3:-}
    expected_run_attempt=${4:-}

    [ -f "$file" ] || fail "missing provenance file: $file"

    awk -F= \
        -v version="$VERSION" \
        -v sha="$SHA" \
        -v expected_repository="$expected_repository" \
        -v expected_run_id="$expected_run_id" \
        -v expected_run_attempt="$expected_run_attempt" '
        function valid_repository(value) {
            return value ~ /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/
        }
        function valid_number(value) {
            return value ~ /^[1-9][0-9]*$/
        }
        $1 == "format" && NF == 2 && $2 == "1" { seen_format++; next }
        $1 == "version" && NF == 2 && $2 == version { seen_version++; next }
        $1 == "source_repository" && NF == 2 && valid_repository($2) {
            repository=$2
            seen_repository++
            next
        }
        $1 == "source_commit" && NF == 2 && $2 == sha { seen_commit++; next }
        $1 == "tests_run_id" && NF == 2 && valid_number($2) {
            run_id=$2
            seen_run_id++
            next
        }
        $1 == "tests_run_attempt" && NF == 2 && valid_number($2) {
            run_attempt=$2
            seen_run_attempt++
            next
        }
        { invalid=1 }
        END {
            if (invalid || NR != 6 || seen_format != 1 || seen_version != 1 ||
                    seen_repository != 1 || seen_commit != 1 ||
                    seen_run_id != 1 || seen_run_attempt != 1)
                exit 1
            if (expected_repository != "" && repository != expected_repository)
                exit 1
            if (expected_run_id != "" && run_id != expected_run_id)
                exit 1
            if (expected_run_attempt != "" && run_attempt != expected_run_attempt)
                exit 1
        }
    ' "$file" || fail "invalid provenance file: $file"
}

provenance_value() {
    key=$1
    file=$2
    sed -n "s/^${key}=//p" "$file"
}

# Create the ephemeral local tag without moving an existing tag.
create_local_release_tag() {
    validate_release_inputs

    if git rev-parse --verify --quiet "refs/tags/$TAG" >/dev/null; then
        existing_sha=$(git rev-list -n 1 "$TAG")
        [ "$existing_sha" = "$SHA" ] ||
            fail "local tag $TAG points to $existing_sha instead of $SHA"
    else
        git tag "$TAG" "$SHA"
    fi

    CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
        ./scripts/version.sh validate-release >/dev/null
}

prepare_installer() {
    source=$1
    destination=$2
    sed \
        -e "s|@CUP_RELEASE_VERSION@|$VERSION|g" \
        -e "s|@CUP_RELEASE_TAG@|$TAG|g" \
        -e "s|@CUP_RELEASE_COMMIT@|$SHA|g" \
        "$source" > "$destination"
}

verify_checksums() {
    directory=$1
    shift

    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$directory" && sha256sum -c "$@")
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$directory" && shasum -a 256 -c "$@")
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

# Verify both checksum contents and the exact authorized asset set.
verify_checksum_file_exact() {
    directory=$1
    checksum_file=$2
    shift 2
    path=$directory/$checksum_file
    [ -f "$path" ] || fail "missing checksum file: $checksum_file"

    entry_count=$(awk 'NF > 0 { count++ } END { print count + 0 }' "$path")
    [ "$entry_count" -eq "$#" ] ||
        fail "checksum file $checksum_file has $entry_count entries; expected $#"

    for expected in "$@"; do
        matches=$(awk -v name="$expected" '
            /^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ {
                file=$0
                sub(/^[0-9a-fA-F]{64}[[:space:]]+\*?/, "", file)
                if (file == name) count++
            }
            END { print count + 0 }
        ' "$path")
        [ "$matches" -eq 1 ] ||
            fail "checksum entry is missing or duplicated in $checksum_file: $expected"
    done

    awk '
        /^[[:space:]]*$/ { next }
        !/^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ { exit 2 }
        {
            file=$0
            sub(/^[0-9a-fA-F]{64}[[:space:]]+\*?/, "", file)
            if (file ~ /(^|\/|\\)\.\.($|\/|\\)/ || file ~ /^\// || file ~ /^[A-Za-z]:/ || file ~ /\\/) exit 3
        }
    ' "$path" || fail "checksum file contains invalid or unsafe entries: $checksum_file"

    verify_checksums "$directory" "$checksum_file"
}
