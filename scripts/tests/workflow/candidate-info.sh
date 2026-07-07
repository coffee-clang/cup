#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

dist=${1:-candidate}
[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'
[ -f "$dist/candidate.env" ] || fail "missing candidate metadata: $dist/candidate.env"

VERSION=
TAG=
SHA=
while IFS='=' read -r key value; do
    case "$key" in
        VERSION) VERSION=$value ;;
        TAG) TAG=$value ;;
        SHA) SHA=$value ;;
        '') ;;
        *) fail "unexpected candidate metadata key: $key" ;;
    esac
done < "$dist/candidate.env"

validate_release_inputs

test "$(sed -n 's/^version=//p' "$dist/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$dist/release.txt")" = "$SHA"
verify_checksums "$dist" SHA256SUMS.common

{
    printf 'version=%s\n' "$VERSION"
    printf 'tag=%s\n' "$TAG"
    printf 'sha=%s\n' "$SHA"
} >> "$GITHUB_OUTPUT"

info "Testing release candidate $TAG at $SHA."
