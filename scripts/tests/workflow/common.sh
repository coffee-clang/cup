#!/usr/bin/env sh
set -eu

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

create_local_release_tag() {
    validate_release_inputs
    git tag -f "$TAG" "$SHA"
    CUP_RELEASE_BUILD=1 ./scripts/version.sh validate-release >/dev/null
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
