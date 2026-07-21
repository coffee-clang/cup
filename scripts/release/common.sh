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

checksum_command() {
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s\n' sha256sum
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s\n' 'shasum -a 256'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

hash_file() {
    checksum=$(checksum_command)
    $checksum "$1" | awk '{print $1}'
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
    checksum=$(checksum_command)
    (cd "$directory" && $checksum -c "$@")
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
