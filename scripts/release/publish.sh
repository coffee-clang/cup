#!/bin/sh

# Publishes only one immutable, provenance-recognizable release snapshot.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
. "$SCRIPT_DIR/common.sh"

: "${GH_REPO:?GH_REPO is required}"
: "${GH_TOKEN:?GH_TOKEN is required}"
: "${SOURCE_REPOSITORY:?SOURCE_REPOSITORY is required}"
: "${TESTS_RUN_ID:?TESTS_RUN_ID is required}"
: "${TESTS_RUN_ATTEMPT:?TESTS_RUN_ATTEMPT is required}"
: "${TESTS_EVIDENCE_INDEX_SHA256:?TESTS_EVIDENCE_INDEX_SHA256 is required}"
: "${RELEASE_RUN_ID:?RELEASE_RUN_ID is required}"
: "${RELEASE_RUN_ATTEMPT:?RELEASE_RUN_ATTEMPT is required}"
validate_release_inputs
validate_repository_identifier "$GH_REPO" GH_REPO
validate_repository_identifier "$SOURCE_REPOSITORY" SOURCE_REPOSITORY
for run_value in "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$RELEASE_RUN_ID" "$RELEASE_RUN_ATTEMPT"; do
    printf '%s\n' "$run_value" | grep -Eq '^[1-9][0-9]*$' || fail 'invalid release provenance run identity'
done
printf '%s\n' "$TESTS_EVIDENCE_INDEX_SHA256" | grep -Eq '^[0-9a-f]{64}$' ||
    fail 'invalid TESTS_EVIDENCE_INDEX_SHA256'

candidate=${1:-dist/candidate}
require_real_directory "$candidate"
public_assets='packages.cfg
install.cfg
release.txt
provenance.txt
THIRD_PARTY_NOTICES.txt
install.sh
install.ps1
cup-linux-x64
cup-linux-arm64
cup-macos-x64
cup-macos-arm64
cup-windows-x64.exe
SHA256SUMS.common
SHA256SUMS.linux-x64
SHA256SUMS.linux-arm64
SHA256SUMS.macos-x64
SHA256SUMS.macos-arm64
SHA256SUMS.windows-x64'
# shellcheck disable=SC2086
validate_exact_directory_files "$candidate" $public_assets
for asset in $public_assets; do require_nonempty_file "$candidate/$asset"; done
# shellcheck disable=SC2086
validate_release_asset_modes "$candidate" $public_assets

state_dir=$(mktemp -d "${TMPDIR:-/tmp}/cup-release-state.XXXXXX")
cleanup_publish() { cup_path_remove_directory_tree "$state_dir" 'publish state directory'; }
trap cleanup_publish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
snapshot=$state_dir/candidate
cup_path_prepare_directory_chain "$snapshot" 'private release snapshot'
chmod 0700 "$snapshot"
for asset in $public_assets; do
    mode=$(release_asset_mode "$asset")
    cup_path_copy_file "$candidate/$asset" "$snapshot/$asset" "$mode" replace ||
        fail "could not snapshot release asset: $asset"
done
# shellcheck disable=SC2086
validate_exact_directory_files "$snapshot" $public_assets
# shellcheck disable=SC2086
validate_release_asset_modes "$snapshot" $public_assets

validate_release_file "$snapshot/release.txt"
validate_provenance_file "$snapshot/provenance.txt" "$SOURCE_REPOSITORY" \
    "$TESTS_RUN_ID" "$TESTS_RUN_ATTEMPT" "$TESTS_EVIDENCE_INDEX_SHA256" \
    "$RELEASE_RUN_ID" "$RELEASE_RUN_ATTEMPT"
grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" "$snapshot/install.sh" >/dev/null
grep -F "CUP_RELEASE_TAG=\"$TAG\"" "$snapshot/install.sh" >/dev/null
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" "$snapshot/install.sh" >/dev/null
grep -F "\$ReleaseVersion = \"$VERSION\"" "$snapshot/install.ps1" >/dev/null
grep -F "\$ReleaseTag = \"$TAG\"" "$snapshot/install.ps1" >/dev/null
grep -F "\$ReleaseCommit = \"$SHA\"" "$snapshot/install.ps1" >/dev/null
! grep -E '@CUP_RELEASE_(VERSION|TAG|COMMIT)@' "$snapshot/install.sh" "$snapshot/install.ps1" >/dev/null
verify_checksum_file_exact "$snapshot" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$snapshot" SHA256SUMS.linux-x64 cup-linux-x64 release.txt SHA256SUMS.common
verify_checksum_file_exact "$snapshot" SHA256SUMS.linux-arm64 cup-linux-arm64 release.txt SHA256SUMS.common
verify_checksum_file_exact "$snapshot" SHA256SUMS.macos-x64 \
    cup-macos-x64 release.txt SHA256SUMS.common
verify_checksum_file_exact "$snapshot" SHA256SUMS.macos-arm64 \
    cup-macos-arm64 release.txt SHA256SUMS.common
verify_checksum_file_exact "$snapshot" SHA256SUMS.windows-x64 \
    cup-windows-x64.exe release.txt SHA256SUMS.common

if [ "$GH_REPO" = "$SOURCE_REPOSITORY" ]; then
    release_target=$SHA
else
    : "${RELEASE_TARGET:?RELEASE_TARGET is required when GH_REPO differs from SOURCE_REPOSITORY}"
    release_target=$RELEASE_TARGET
fi
canonical_target=$(gh api "repos/$GH_REPO/commits/$release_target" --jq '.sha') ||
    fail "could not resolve release target '$release_target' in $GH_REPO"
printf '%s\n' "$canonical_target" | grep -Eq '^[0-9a-f]{40}$' || fail 'release target did not resolve to a commit'
if [ "$GH_REPO" = "$SOURCE_REPOSITORY" ]; then
    [ "$canonical_target" = "$SHA" ] || fail 'release target does not match the tested source commit'
fi

api_optional_value() {
    api_endpoint=$1
    api_filter=$2
    api_error=$3
    API_OPTIONAL_VALUE=
    if api_value=$(gh api "$api_endpoint" --jq "$api_filter" 2>"$api_error"); then
        API_OPTIONAL_VALUE=$api_value
        return 0
    fi
    if grep -Eq '(^|[^0-9])404([^0-9]|$)|Not Found' "$api_error"; then
        return 1
    fi
    cat "$api_error" >&2 || true
    return 2
}

query_tag() {
    TAG_EXISTS=0
    tag_error=$state_dir/tag.error
    if api_optional_value "repos/$GH_REPO/git/ref/tags/$TAG" '.object.sha' "$tag_error"; then
        TAG_EXISTS=1
        tag_commit_error=$state_dir/tag-commit.error
        if ! TAG_COMMIT=$(gh api "repos/$GH_REPO/commits/$TAG" --jq '.sha' 2>"$tag_commit_error"); then
            cat "$tag_commit_error" >&2 || true
            fail "could not resolve tag $TAG to a commit"
        fi
        printf '%s\n' "$TAG_COMMIT" | grep -Eq '^[0-9a-f]{40}$' || fail "could not read tag commit for $TAG"
        [ "$TAG_COMMIT" = "$canonical_target" ] ||
            fail "tag $TAG points to ${TAG_COMMIT:-an unknown commit}, expected $canonical_target"
    else
        status=$?
        [ "$status" -eq 1 ] || fail "GitHub API request failed while resolving tag $TAG"
    fi
}

query_release() {
    RELEASE_EXISTS=0
    RELEASE_IS_DRAFT=
    RELEASE_TARGETISH=
    release_error=$state_dir/release.error
    if ! release_ids=$(gh api "repos/$GH_REPO/releases?per_page=100" --paginate \
            --jq ".[] | select(.tag_name == \"$TAG\") | .id" 2>"$release_error"); then
        cat "$release_error" >&2 || true
        fail "GitHub API request failed while resolving release $TAG"
    fi
    release_count=$(printf '%s\n' "$release_ids" | awk 'NF { count++ } END { print count + 0 }')
    [ "$release_count" -le 1 ] || fail "multiple releases found for tag $TAG"
    [ "$release_count" -eq 1 ] || return 0

    RELEASE_EXISTS=1
    release_id=$release_ids
    release_detail_error=$state_dir/release-detail.error
    if ! release_details=$(gh api "repos/$GH_REPO/releases/$release_id" \
            --jq '[.draft, .target_commitish] | @tsv' 2>"$release_detail_error"); then
        cat "$release_detail_error" >&2 || true
        fail "could not read release state for $TAG"
    fi
    tab=$(printf '\t')
    case "$release_details" in
        *"$tab"*)
            RELEASE_IS_DRAFT=${release_details%%"$tab"*}
            RELEASE_TARGETISH=${release_details#*"$tab"}
            ;;
        *) fail "could not determine release state for $TAG" ;;
    esac
    case "$RELEASE_IS_DRAFT" in true|false) ;; *) fail "could not determine release state for $TAG" ;; esac
}

validate_release_binding() {
    [ "$RELEASE_EXISTS" -eq 1 ] || return 0
    if [ "$TAG_EXISTS" -eq 1 ]; then
        return 0
    fi
    if [ "$RELEASE_IS_DRAFT" = false ]; then
        query_tag
        [ "$TAG_EXISTS" -eq 1 ] || fail "published release $TAG exists without a resolvable tag"
        return 0
    fi

    [ -n "$RELEASE_TARGETISH" ] || fail "draft $TAG has no release target"
    draft_target_error=$state_dir/draft-target.error
    if ! draft_target=$(gh api "repos/$GH_REPO/commits/$RELEASE_TARGETISH" \
            --jq '.sha' 2>"$draft_target_error"); then
        cat "$draft_target_error" >&2 || true
        fail "could not resolve draft target for $TAG"
    fi
    [ "$draft_target" = "$canonical_target" ] ||
        fail "draft $TAG targets $draft_target, expected $canonical_target"
}

expected_assets=$(printf '%s\n' $public_assets | LC_ALL=C sort)
set --
for asset in $public_assets; do set -- "$@" "$snapshot/$asset"; done

verify_remote_assets() (
    remote_dir=$(mktemp -d "${TMPDIR:-/tmp}/cup-release-assets.XXXXXX")
    cleanup_remote_assets() { cup_path_remove_directory_tree "$remote_dir" 'remote release verification directory'; }
    trap cleanup_remote_assets EXIT
    actual_assets=$(gh release view "$TAG" --repo "$GH_REPO" --json assets --jq '.assets[].name' | LC_ALL=C sort) ||
        fail "could not query release assets for $TAG"
    [ "$actual_assets" = "$expected_assets" ] || {
        printf 'Expected release assets:\n%s\n' "$expected_assets" >&2
        printf 'Actual release assets:\n%s\n' "$actual_assets" >&2
        fail "release $TAG has an incomplete or unexpected asset set"
    }
    gh release download "$TAG" --repo "$GH_REPO" --dir "$remote_dir" --clobber ||
        fail "could not download release $TAG for verification"
    for remote_asset in $public_assets; do
        require_nonempty_file "$remote_dir/$remote_asset"
        [ "$(hash_file "$remote_dir/$remote_asset")" = "$(hash_file "$snapshot/$remote_asset")" ] ||
            fail "release asset differs from the immutable candidate: $remote_asset"
    done
)

require_recognizable_draft() (
    provenance_dir=$(mktemp -d "${TMPDIR:-/tmp}/cup-release-provenance.XXXXXX")
    cleanup_provenance() { cup_path_remove_directory_tree "$provenance_dir" 'draft provenance directory'; }
    trap cleanup_provenance EXIT
    assets=$(gh release view "$TAG" --repo "$GH_REPO" --json assets --jq '.assets[].name') ||
        fail "could not query draft assets for $TAG"
    printf '%s\n' "$assets" | grep -Fx provenance.txt >/dev/null ||
        fail "draft $TAG has no recognizable provenance; preserving it unchanged"
    gh release download "$TAG" --repo "$GH_REPO" --dir "$provenance_dir" \
        --pattern provenance.txt --clobber ||
        fail "could not download draft provenance for $TAG"
    require_nonempty_file "$provenance_dir/provenance.txt"
    cmp -s "$provenance_dir/provenance.txt" "$snapshot/provenance.txt" ||
        fail "draft $TAG provenance does not match this release attempt; preserving it unchanged"
)

query_tag
query_release
validate_release_binding
if [ "$RELEASE_EXISTS" -eq 1 ] && [ "$RELEASE_IS_DRAFT" = false ]; then
    verify_remote_assets
    info "Release $TAG is already published with the verified asset set."
    exit 0
fi

if [ "$RELEASE_EXISTS" -eq 0 ]; then
    # Recheck immediately before creation so a concurrent publisher becomes
    # an observable draft/published release rather than an implicit conflict.
    query_release
fi
if [ "$RELEASE_EXISTS" -eq 0 ]; then
    if [ "$TAG_EXISTS" -eq 1 ]; then
        info "Creating draft release $TAG from the existing verified tag."
        if ! gh release create "$TAG" "$@" --repo "$GH_REPO" --verify-tag \
                --title "cup $VERSION" --generate-notes --draft; then
            query_release
            [ "$RELEASE_EXISTS" -eq 1 ] || fail "could not create draft release $TAG"
        fi
    else
        info "Creating draft release $TAG on commit $canonical_target."
        if ! gh release create "$TAG" "$@" --repo "$GH_REPO" --target "$canonical_target" \
                --title "cup $VERSION" --generate-notes --draft; then
            query_release
            [ "$RELEASE_EXISTS" -eq 1 ] || fail "could not create draft release $TAG"
        fi
    fi
    query_tag
    query_release
    validate_release_binding
fi

if [ "$RELEASE_IS_DRAFT" = false ]; then
    verify_remote_assets
    info "A concurrent publisher completed release $TAG with the verified asset set."
    exit 0
fi
require_recognizable_draft
info "Reconciling recognized draft release $TAG."
current_assets=$(gh release view "$TAG" --repo "$GH_REPO" --json assets --jq '.assets[].name') ||
    fail "could not query draft assets for $TAG"
printf '%s\n' "$current_assets" | while IFS= read -r current_asset; do
    [ -n "$current_asset" ] || continue
    expected=0
    for expected_asset in $public_assets; do
        [ "$current_asset" != "$expected_asset" ] || { expected=1; break; }
    done
    [ "$expected" -eq 1 ] || gh release delete-asset "$TAG" "$current_asset" --repo "$GH_REPO" --yes
 done
gh release upload "$TAG" "$@" --repo "$GH_REPO" --clobber || fail "could not upload release snapshot for $TAG"
verify_remote_assets

# State, target and bytes are checked again immediately before publication.
# A concurrent completion is accepted only when the published bytes are exact.
query_tag
query_release
[ "$RELEASE_EXISTS" -eq 1 ] || fail "release $TAG disappeared before publication"
validate_release_binding
if [ "$RELEASE_IS_DRAFT" = false ]; then
    verify_remote_assets
    info "A concurrent publisher completed release $TAG with the verified asset set."
    exit 0
fi
require_recognizable_draft
verify_remote_assets
gh release edit "$TAG" --repo "$GH_REPO" --draft=false --latest || fail "could not publish release $TAG"
query_tag
query_release
[ "$RELEASE_EXISTS" -eq 1 ] && [ "$RELEASE_IS_DRAFT" = false ] ||
    fail "release $TAG did not become published"
validate_release_binding
verify_remote_assets
info "Published release $TAG."
