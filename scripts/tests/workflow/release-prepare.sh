#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'

version=$(./scripts/version.sh base)
tag="v$version"
sha=$(git rev-parse HEAD)
should_release=1

VERSION=$version TAG=$tag SHA=$sha validate_release_inputs

if git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null 2>&1; then
    should_release=0
    info "Remote tag $tag already exists; no release candidate is needed."
else
    info "Release candidate detected: $tag at $sha."
fi

{
    printf 'version=%s\n' "$version"
    printf 'tag=%s\n' "$tag"
    printf 'sha=%s\n' "$sha"
    printf 'should_release=%s\n' "$should_release"
} >> "$GITHUB_OUTPUT"
