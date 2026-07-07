#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'

version=$(./scripts/version.sh base)
tag="v$version"
sha=$(git rev-parse HEAD)

VERSION=$version TAG=$tag SHA=$sha validate_release_inputs

if git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null 2>&1; then
    fail "remote tag $tag already exists; refusing to build duplicate release assets"
fi

{
    printf 'version=%s\n' "$version"
    printf 'tag=%s\n' "$tag"
    printf 'sha=%s\n' "$sha"
} >> "$GITHUB_OUTPUT"

info "Release candidate: $tag at $sha."
