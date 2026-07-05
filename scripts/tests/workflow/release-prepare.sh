#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'

version=$(./scripts/version.sh base)
tag="v$version"
sha=$(git rev-parse HEAD)

release_needed=1
if git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null 2>&1; then
    release_needed=0
fi

{
    printf 'version=%s\n' "$version"
    printf 'tag=%s\n' "$tag"
    printf 'sha=%s\n' "$sha"
    printf 'release_needed=%s\n' "$release_needed"
} >> "$GITHUB_OUTPUT"

if [ "$release_needed" -eq 1 ]; then
    info "Release candidate detected: $tag at $sha."
else
    info "Tag $tag already exists; release workflow will stop after this check."
fi
