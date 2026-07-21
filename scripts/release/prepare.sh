#!/usr/bin/env sh

# Purpose: Resolves VERSION, tag, commit and whether Tests must assemble a
# release candidate for the public release repository.
set -eu

. "$(dirname "$0")/common.sh"

[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'
: "${RELEASE_REPOSITORY:?RELEASE_REPOSITORY is required}"

is_main=1
if [ "${GITHUB_ACTIONS:-}" = true ] && [ "${GITHUB_REF:-}" != refs/heads/main ]; then
    is_main=0
fi

# Resolve the tested source identity before consulting public release state.
version=$(./scripts/version.sh base)
tag="v$version"
sha=$(git rev-parse HEAD)
should_release=1

VERSION=$version TAG=$tag SHA=$sha validate_release_inputs

# Only main-branch runs may produce a release candidate.
if [ "$is_main" -eq 0 ]; then
    should_release=0
    info "Manual Tests run is not on main; release-candidate jobs will be skipped."
else
    : "${GH_TOKEN:?GH_TOKEN is required to inspect the public release repository}"
    query_error=$(mktemp "${TMPDIR:-/tmp}/cup-release-query.XXXXXX") ||
        fail 'could not create a temporary release query file'
    if release_is_draft=$(gh release view "$tag" --repo "$RELEASE_REPOSITORY" \
            --json isDraft --jq '.isDraft' 2>"$query_error"); then
        case "$release_is_draft" in
            false)
                should_release=0
                info "Public release $tag already exists; source tests will run without rebuilding it."
                ;;
            true)
                info "Draft release $tag exists; a fresh verified candidate will be assembled."
                ;;
            *)
                rm -f "$query_error"
                fail "could not determine whether release $tag is a draft"
                ;;
        esac
    elif grep -Eiq 'release not found|HTTP 404|status code 404' "$query_error"; then
        info "Release candidate detected: $tag at $sha."
    else
        cat "$query_error" >&2
        rm -f "$query_error"
        fail "could not query public release $tag in $RELEASE_REPOSITORY"
    fi
    rm -f "$query_error"
fi

{
    printf 'version=%s\n' "$version"
    printf 'tag=%s\n' "$tag"
    printf 'sha=%s\n' "$sha"
    printf 'should_release=%s\n' "$should_release"
} >> "$GITHUB_OUTPUT"
