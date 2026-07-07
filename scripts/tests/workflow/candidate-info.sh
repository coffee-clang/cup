#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

dist=${1:-candidate}
[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'

read_env_file() {
    file=$1
    while IFS='=' read -r key value; do
        case "$key" in
            SHOULD_RELEASE) SHOULD_RELEASE=$value ;;
            VERSION) VERSION=$value ;;
            TAG) TAG=$value ;;
            SHA) SHA=$value ;;
            '') ;;
            *) fail "unexpected candidate metadata key in $file: $key" ;;
        esac
    done < "$file"
}

SHOULD_RELEASE=
VERSION=
TAG=
SHA=

if [ -f "$dist/release-decision.env" ]; then
    read_env_file "$dist/release-decision.env"
else
    SHOULD_RELEASE=1
fi

case "$SHOULD_RELEASE" in
    0|1) ;;
    *) fail "invalid SHOULD_RELEASE value: $SHOULD_RELEASE" ;;
esac

if [ "$SHOULD_RELEASE" = 0 ]; then
    validate_release_inputs
    {
        printf 'should_release=0\n'
        printf 'version=%s\n' "$VERSION"
        printf 'tag=%s\n' "$TAG"
        printf 'sha=%s\n' "$SHA"
    } >> "$GITHUB_OUTPUT"
    info "No release is needed for $TAG; the tag already exists."
    exit 0
fi

[ -f "$dist/candidate.env" ] || fail "missing candidate metadata: $dist/candidate.env"
if [ -z "$VERSION" ] && [ -z "$TAG" ] && [ -z "$SHA" ]; then
    read_env_file "$dist/candidate.env"
else
    candidate_version=
    candidate_tag=
    candidate_sha=
    while IFS='=' read -r key value; do
        case "$key" in
            VERSION) candidate_version=$value ;;
            TAG) candidate_tag=$value ;;
            SHA) candidate_sha=$value ;;
            '') ;;
            *) fail "unexpected candidate metadata key in $dist/candidate.env: $key" ;;
        esac
    done < "$dist/candidate.env"
    [ "$candidate_version" = "$VERSION" ] || fail 'candidate VERSION does not match release decision'
    [ "$candidate_tag" = "$TAG" ] || fail 'candidate TAG does not match release decision'
    [ "$candidate_sha" = "$SHA" ] || fail 'candidate SHA does not match release decision'
fi

validate_release_inputs

test "$(sed -n 's/^version=//p' "$dist/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$dist/release.txt")" = "$SHA"
verify_checksums "$dist" SHA256SUMS.common

{
    printf 'should_release=1\n'
    printf 'version=%s\n' "$VERSION"
    printf 'tag=%s\n' "$TAG"
    printf 'sha=%s\n' "$SHA"
} >> "$GITHUB_OUTPUT"

info "Testing release candidate $TAG at $SHA."
