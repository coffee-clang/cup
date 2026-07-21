#!/usr/bin/env sh

# Purpose: Resumes or creates a draft, reconciles assets, and publishes only
# an exact verified candidate.
# Inputs: candidate directory plus VERSION, TAG, source SHA, GH_TOKEN and public GH_REPO.
set -eu

. "$(dirname "$0")/common.sh"

: "${GH_REPO:?GH_REPO is required}"
: "${GH_TOKEN:?GH_TOKEN is required}"
: "${SOURCE_REPOSITORY:?SOURCE_REPOSITORY is required}"
: "${TESTS_RUN_ID:?TESTS_RUN_ID is required}"
: "${TESTS_RUN_ATTEMPT:?TESTS_RUN_ATTEMPT is required}"

dist=${1:-dist}
validate_release_inputs

# Public asset allowlist. Internal decision files never cross the release boundary.
public_assets="
packages.cfg
install.cfg
release.txt
provenance.txt
THIRD_PARTY_DEPENDENCIES.txt
install.sh
install.ps1
uninstall.sh
uninstall.ps1
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
SHA256SUMS.windows-x64
"

for asset in candidate.env $public_assets; do
    [ -f "$dist/$asset" ] || fail "missing asset: $asset"
done

# Internal CI markers must not become public release assets.
[ -f "$dist/release-decision.env" ] ||
    fail 'missing internal release decision metadata'

test "$(sed -n 's/^format=//p' "$dist/release.txt")" = 1
test "$(sed -n 's/^version=//p' "$dist/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$dist/release.txt")" = "$SHA"
test "$(wc -l < "$dist/release.txt" | tr -d '[:space:]')" = 3

awk -F= \
    -v version="$VERSION" \
    -v sha="$SHA" \
    -v source_repository="$SOURCE_REPOSITORY" \
    -v tests_run_id="$TESTS_RUN_ID" \
    -v tests_run_attempt="$TESTS_RUN_ATTEMPT" '
    function valid_repository(value) {
        return value ~ /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/
    }
    function valid_number(value) {
        return value ~ /^[1-9][0-9]*$/
    }
    $1 == "format" && NF == 2 && $2 == "1" { seen_format++; next }
    $1 == "version" && NF == 2 && $2 == version { seen_version++; next }
    $1 == "source_repository" && NF == 2 && valid_repository($2) &&
            $2 == source_repository {
        seen_repository++
        next
    }
    $1 == "source_commit" && NF == 2 && $2 == sha { seen_commit++; next }
    $1 == "tests_run_id" && NF == 2 && valid_number($2) &&
            $2 == tests_run_id { seen_run_id++; next }
    $1 == "tests_run_attempt" && NF == 2 && valid_number($2) &&
            $2 == tests_run_attempt {
        seen_run_attempt++
        next
    }
    { invalid=1 }
    END {
        if (invalid || NR != 6 || seen_format != 1 || seen_version != 1 ||
                seen_repository != 1 || seen_commit != 1 ||
                seen_run_id != 1 || seen_run_attempt != 1)
            exit 1
    }
' "$dist/provenance.txt" || fail 'candidate provenance is invalid'

awk -F= -v version="$VERSION" -v tag="$TAG" -v sha="$SHA" '
    $1 == "VERSION" && NF == 2 && $2 == version { seen_version++; next }
    $1 == "TAG" && NF == 2 && $2 == tag { seen_tag++; next }
    $1 == "SHA" && NF == 2 && $2 == sha { seen_sha++; next }
    { invalid=1 }
    END {
        if (invalid || seen_version != 1 || seen_tag != 1 || seen_sha != 1)
            exit 1
    }
' "$dist/candidate.env" || fail 'candidate metadata does not match the release inputs'
awk -F= -v version="$VERSION" -v tag="$TAG" -v sha="$SHA" '
    $1 == "SHOULD_RELEASE" && NF == 2 && $2 == "1" { seen_decision++; next }
    $1 == "VERSION" && NF == 2 && $2 == version { seen_version++; next }
    $1 == "TAG" && NF == 2 && $2 == tag { seen_tag++; next }
    $1 == "SHA" && NF == 2 && $2 == sha { seen_sha++; next }
    { invalid=1 }
    END {
        if (invalid || seen_decision != 1 || seen_version != 1 ||
                seen_tag != 1 || seen_sha != 1)
            exit 1
    }
' "$dist/release-decision.env" ||
    fail 'release decision metadata does not match the verified candidate'
grep -F "CUP_RELEASE_VERSION=\"$VERSION\"" "$dist/install.sh"
grep -F "CUP_RELEASE_TAG=\"$TAG\"" "$dist/install.sh"
grep -F "CUP_RELEASE_COMMIT=\"$SHA\"" "$dist/install.sh"
grep -F "\$ReleaseVersion = \"$VERSION\"" "$dist/install.ps1"
grep -F "\$ReleaseTag = \"$TAG\"" "$dist/install.ps1"
grep -F "\$ReleaseCommit = \"$SHA\"" "$dist/install.ps1"
! grep -R '@CUP_RELEASE_' "$dist"

verify_checksum_file_exact "$dist" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$dist" SHA256SUMS.linux-x64 \
    cup-linux-x64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.linux-arm64 \
    cup-linux-arm64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.macos-x64 \
    cup-macos-x64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.macos-arm64 \
    cup-macos-arm64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.windows-x64 \
    cup-windows-x64.exe uninstall.ps1 release.txt

set --
for asset in $public_assets; do
    set -- "$@" "$dist/$asset"
done

# Discover tag and release state without changing the public repository.
release_repository_url="https://github.com/$GH_REPO.git"
if ! remote_tag_output=$(git ls-remote --tags "$release_repository_url" \
        "refs/tags/$TAG" "refs/tags/$TAG^{}"); then
    fail "could not query public release tag $TAG in $GH_REPO"
fi
remote_tag_exists=0
[ -z "$remote_tag_output" ] || remote_tag_exists=1

# The public download repository is intentionally separate from the private
# source repository. Its tag commit therefore cannot identify the tested
# source revision; release.txt and the verified Tests artifacts carry SHA.
release_exists=0
release_is_draft=false
release_query_error=$(mktemp "${TMPDIR:-/tmp}/cup-release-query.XXXXXX") ||
    fail 'could not create a temporary release query file'
if release_is_draft=$(gh release view "$TAG" --repo "$GH_REPO" \
        --json isDraft --jq '.isDraft' 2>"$release_query_error"); then
    release_exists=1
    [ "$remote_tag_exists" -eq 1 ] || {
        rm -f "$release_query_error"
        fail "release $TAG exists without its public repository tag"
    }
    case "$release_is_draft" in
        true|false) ;;
        *)
            rm -f "$release_query_error"
            fail "could not determine whether release $TAG is a draft"
            ;;
    esac
elif ! grep -Eiq 'release not found|HTTP 404|status code 404' \
        "$release_query_error"; then
    cat "$release_query_error" >&2
    rm -f "$release_query_error"
    fail "could not query release $TAG"
fi
rm -f "$release_query_error"

# Exact public asset set for remote verification.
expected_assets=$(printf '%s\n' $public_assets | LC_ALL=C sort)
# Download and byte-compare the remote release before acceptance.
# Compare the remote asset set and every downloaded byte with the candidate.
verify_remote_assets() {
    actual_assets=$(gh release view "$TAG" --repo "$GH_REPO" \
        --json assets --jq '.assets[].name' | LC_ALL=C sort)
    if [ "$actual_assets" != "$expected_assets" ]; then
        printf 'Expected release assets:\n%s\n' "$expected_assets" >&2
        printf 'Actual release assets:\n%s\n' "$actual_assets" >&2
        fail "release $TAG has an incomplete or unexpected asset set"
    fi

    download_dir=$(mktemp -d "${TMPDIR:-/tmp}/cup-release-assets.XXXXXX") ||
        fail 'could not create a temporary release verification directory'
    if ! gh release download "$TAG" --repo "$GH_REPO" \
            --dir "$download_dir" --clobber; then
        rm -rf "$download_dir"
        fail "could not download release $TAG for verification"
    fi
    for asset in $public_assets; do
        if [ ! -f "$download_dir/$asset" ]; then
            rm -rf "$download_dir"
            fail "release asset is missing after download: $asset"
        fi
        remote_hash=$(hash_file "$download_dir/$asset")
        candidate_hash=$(hash_file "$dist/$asset")
        if [ "$remote_hash" != "$candidate_hash" ]; then
            rm -rf "$download_dir"
            fail "release asset differs from the verified candidate: $asset"
        fi
    done
    rm -rf "$download_dir"
}

# Resume or create a draft; publication occurs only after byte verification.
if [ "$release_exists" -eq 1 ] && [ "$release_is_draft" = false ]; then
    verify_remote_assets
    info "Release $TAG is already published with the verified asset set."
    exit 0
fi

if [ "$release_exists" -eq 1 ]; then
    info "Resuming draft release $TAG."
    current_assets=$(gh release view "$TAG" --repo "$GH_REPO" \
        --json assets --jq '.assets[].name')
    printf '%s\n' "$current_assets" | while IFS= read -r asset; do
        [ -n "$asset" ] || continue
        is_expected=0
        for expected_asset in $public_assets; do
            if [ "$asset" = "$expected_asset" ]; then
                is_expected=1
                break
            fi
        done
        if [ "$is_expected" -eq 0 ]; then
            gh release delete-asset "$TAG" "$asset" \
                --repo "$GH_REPO" --yes
        fi
    done
    gh release upload "$TAG" "$@" --repo "$GH_REPO" --clobber
elif [ "$remote_tag_exists" -eq 1 ]; then
    info "Creating draft release $TAG from the existing public tag."
    gh release create "$TAG" "$@" --repo "$GH_REPO" \
        --verify-tag --title "cup $VERSION" --generate-notes --draft
else
    info "Creating draft release $TAG for tested source commit $SHA."
    gh release create "$TAG" "$@" --repo "$GH_REPO" \
        --title "cup $VERSION" --generate-notes --draft
fi

verify_remote_assets
gh release edit "$TAG" --repo "$GH_REPO" --draft=false --latest
info "Published release $TAG."
