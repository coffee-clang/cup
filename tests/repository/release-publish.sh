#!/usr/bin/env bash

# Purpose: Exercises resumable, idempotent, and conflict-safe publication
# with simulated git and gh boundaries.
set -euo pipefail

TESTS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$TESTS_ROOT/support/common.sh"
ROOT="$PROJECT_ROOT"
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cup-release-publish-tests.XXXXXX")
trap 'rm -rf "$TMP_ROOT"' EXIT HUP INT TERM

VERSION=1.2.3
TAG=v$VERSION
SHA=0123456789abcdef0123456789abcdef01234567
DIST=$TMP_ROOT/dist
MOCK_BIN=$TMP_ROOT/bin
MOCK_STATE=$TMP_ROOT/state
mkdir -p "$DIST" "$MOCK_BIN" "$MOCK_STATE"

public_assets=(
    packages.cfg install.cfg release.txt provenance.txt THIRD_PARTY_NOTICES.txt
    install.sh install.ps1 uninstall.sh uninstall.ps1
    cup-linux-x64 cup-linux-arm64 cup-macos-x64 cup-macos-arm64
    cup-windows-x64.exe SHA256SUMS.common SHA256SUMS.linux-x64
    SHA256SUMS.linux-arm64 SHA256SUMS.macos-x64
    SHA256SUMS.macos-arm64 SHA256SUMS.windows-x64
)
printf '%s\n' "${public_assets[@]}" > "$MOCK_STATE/expected-assets"

printf 'packages\n' > "$DIST/packages.cfg"
printf 'install configuration\n' > "$DIST/install.cfg"
printf 'format=1\nversion=%s\ncommit=%s\n' "$VERSION" "$SHA" > "$DIST/release.txt"
cat > "$DIST/provenance.txt" <<EOF_PROVENANCE
format=1
version=$VERSION
source_repository=example/cup-source
source_commit=$SHA
tests_run_id=41
tests_run_attempt=2
EOF_PROVENANCE
printf 'third-party licenses\n' > "$DIST/THIRD_PARTY_NOTICES.txt"
printf 'CUP_RELEASE_VERSION="%s"\nCUP_RELEASE_TAG="%s"\nCUP_RELEASE_COMMIT="%s"\n' \
    "$VERSION" "$TAG" "$SHA" > "$DIST/install.sh"
printf '\$ReleaseVersion = "%s"\n\$ReleaseTag = "%s"\n\$ReleaseCommit = "%s"\n' \
    "$VERSION" "$TAG" "$SHA" > "$DIST/install.ps1"
printf 'uninstall\n' > "$DIST/uninstall.sh"
printf 'uninstall\n' > "$DIST/uninstall.ps1"
for platform in linux-x64 linux-arm64 macos-x64 macos-arm64; do
    printf '%s\n' "$platform" > "$DIST/cup-$platform"
done
printf 'windows\n' > "$DIST/cup-windows-x64.exe"
printf 'VERSION=%s\nTAG=%s\nSHA=%s\n' "$VERSION" "$TAG" "$SHA" > "$DIST/candidate.env"
printf 'SHOULD_RELEASE=1\nVERSION=%s\nTAG=%s\nSHA=%s\n' \
    "$VERSION" "$TAG" "$SHA" > "$DIST/release-decision.env"

write_checksums() {
    local output=$1
    shift
    : > "$DIST/$output"
    for name in "$@"; do
        printf '%s  %s\n' "$(hash_file "$DIST/$name")" "$name" \
            >> "$DIST/$output"
    done
}
write_checksums SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
write_checksums SHA256SUMS.linux-x64 cup-linux-x64 uninstall.sh release.txt
write_checksums SHA256SUMS.linux-arm64 cup-linux-arm64 uninstall.sh release.txt
write_checksums SHA256SUMS.macos-x64 cup-macos-x64 uninstall.sh release.txt
write_checksums SHA256SUMS.macos-arm64 cup-macos-arm64 uninstall.sh release.txt
write_checksums SHA256SUMS.windows-x64 cup-windows-x64.exe uninstall.ps1 release.txt

cat > "$MOCK_BIN/git" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = ls-remote ]; then
    printf '%s\n' "$*" > "$MOCK_STATE/git-ls-remote"
    if [ -f "$MOCK_STATE/tag-sha" ]; then
        printf '%s\trefs/tags/%s\n' "$(cat "$MOCK_STATE/tag-sha")" "$TAG"
        exit 0
    fi
    exit 0
fi
printf 'unexpected git invocation: %s\n' "$*" >&2
exit 2
MOCK

cat > "$MOCK_BIN/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:-}" = release ] || exit 2
command=${2:-}
shift 2
case "$command" in
    view)
        if [ -f "$MOCK_STATE/release-view-error" ]; then
            printf 'network unavailable\n' >&2
            exit 1
        fi
        if [ ! -f "$MOCK_STATE/release" ]; then
            printf 'release not found\n' >&2
            exit 1
        fi
        case " $* " in
            *" --json isDraft "*)
                cat "$MOCK_STATE/draft"
                ;;
            *" --json assets "*)
                cat "$MOCK_STATE/assets"
                ;;
            *)
                exit 0
                ;;
        esac
        ;;
    download)
        destination=
        while [ "$#" -gt 0 ]; do
            if [ "$1" = --dir ]; then
                destination=$2
                shift 2
            else
                shift
            fi
        done
        [ -n "$destination" ] || exit 2
        mkdir -p "$destination"
        while IFS= read -r asset; do
            cp "$MOCK_DIST/$asset" "$destination/$asset"
        done < "$MOCK_STATE/assets"
        if [ -f "$MOCK_STATE/corrupt-download" ]; then
            printf 'corrupt\n' >> "$destination/packages.cfg"
        fi
        ;;
    create)
        : > "$MOCK_STATE/release"
        printf 'true\n' > "$MOCK_STATE/draft"
        cp "$MOCK_STATE/expected-assets" "$MOCK_STATE/assets"
        [ -f "$MOCK_STATE/tag-sha" ] || printf '%040d\n' 7 > "$MOCK_STATE/tag-sha"
        printf 'create:%s\n' "$*" >> "$MOCK_STATE/calls"
        ;;
    delete-asset)
        asset=${2:-}
        grep -Fvx "$asset" "$MOCK_STATE/assets" > "$MOCK_STATE/assets.next" || true
        mv "$MOCK_STATE/assets.next" "$MOCK_STATE/assets"
        printf 'delete:%s\n' "$asset" >> "$MOCK_STATE/calls"
        ;;
    upload)
        cp "$MOCK_STATE/expected-assets" "$MOCK_STATE/assets"
        printf 'upload\n' >> "$MOCK_STATE/calls"
        ;;
    edit)
        printf 'false\n' > "$MOCK_STATE/draft"
        printf 'edit\n' >> "$MOCK_STATE/calls"
        ;;
    *)
        exit 2
        ;;
esac
MOCK
chmod +x "$MOCK_BIN/git" "$MOCK_BIN/gh"

run_publish() {
    PATH="$MOCK_BIN:$PATH" MOCK_STATE="$MOCK_STATE" MOCK_DIST="$DIST" \
        VERSION="$VERSION" TAG="$TAG" SHA="$SHA" \
        SOURCE_REPOSITORY=example/cup-source \
        TESTS_RUN_ID=41 TESTS_RUN_ATTEMPT=2 \
        GH_TOKEN=test GH_REPO=example/cup \
        "$ROOT/scripts/release/publish.sh" "$DIST"
}

# Candidate metadata must be exact: duplicate or conflicting keys are not
# accepted merely because one matching line is present.
cp "$DIST/candidate.env" "$TMP_ROOT/candidate.env.valid"
printf 'VERSION=%s\n' "$VERSION" >> "$DIST/candidate.env"
if run_publish > "$TMP_ROOT/duplicate-candidate.out" 2>&1; then
    printf 'duplicate candidate metadata unexpectedly passed validation\n' >&2
    exit 1
fi
grep -F 'candidate metadata does not match' \
    "$TMP_ROOT/duplicate-candidate.out" >/dev/null
mv "$TMP_ROOT/candidate.env.valid" "$DIST/candidate.env"

cp "$DIST/release-decision.env" "$TMP_ROOT/release-decision.env.valid"
printf 'SHA=%s\n' "$SHA" >> "$DIST/release-decision.env"
if run_publish > "$TMP_ROOT/duplicate-decision.out" 2>&1; then
    printf 'duplicate release decision metadata unexpectedly passed validation\n' >&2
    exit 1
fi
grep -F 'release decision metadata does not match' \
    "$TMP_ROOT/duplicate-decision.out" >/dev/null
mv "$TMP_ROOT/release-decision.env.valid" "$DIST/release-decision.env"

run_publish >/dev/null
[ "$(cat "$MOCK_STATE/draft")" = false ]
[ "$(grep -c '^create:' "$MOCK_STATE/calls")" -eq 1 ]
[ "$(grep -c '^edit$' "$MOCK_STATE/calls")" -eq 1 ]

# Query failures must not be mistaken for an absent release.
: > "$MOCK_STATE/release-view-error"
if run_publish > "$TMP_ROOT/query-error.out" 2>&1; then
    printf 'release query failure unexpectedly triggered publication\n' >&2
    exit 1
fi
grep -F 'could not query release' "$TMP_ROOT/query-error.out" >/dev/null
rm -f "$MOCK_STATE/release-view-error"
[ "$(grep -c '^create:' "$MOCK_STATE/calls")" -eq 1 ]

# Re-running a completed publication is a verified no-op.
run_publish >/dev/null
[ "$(grep -c '^create:' "$MOCK_STATE/calls")" -eq 1 ]
[ "$(grep -c '^edit$' "$MOCK_STATE/calls")" -eq 1 ]

# Matching names are not enough: remote bytes must also match the candidate.
: > "$MOCK_STATE/corrupt-download"
if run_publish > "$TMP_ROOT/corrupt.out" 2>&1; then
    printf 'corrupt published asset unexpectedly passed verification\n' >&2
    exit 1
fi
grep -F 'differs from the verified candidate' "$TMP_ROOT/corrupt.out" >/dev/null
rm -f "$MOCK_STATE/corrupt-download"

# A partial draft is completed by replacing every expected asset and removing
# stale public assets left by an interrupted or older publication attempt.
printf 'true\n' > "$MOCK_STATE/draft"
printf 'packages.cfg\nunexpected.bin\n' > "$MOCK_STATE/assets"
run_publish >/dev/null
[ "$(grep -c '^delete:unexpected.bin$' "$MOCK_STATE/calls")" -eq 1 ]
[ "$(grep -c '^upload$' "$MOCK_STATE/calls")" -eq 1 ]
[ "$(grep -c '^edit$' "$MOCK_STATE/calls")" -eq 2 ]

# The public repository tag is intentionally unrelated to the private source
# commit. An existing public tag is reused, while release.txt remains the
# authoritative source-SHA record.
rm -f "$MOCK_STATE/release" "$MOCK_STATE/draft" "$MOCK_STATE/assets"
: > "$MOCK_STATE/calls"
printf '%040d\n' 1 > "$MOCK_STATE/tag-sha"
run_publish >/dev/null
grep -F 'https://github.com/example/cup.git' "$MOCK_STATE/git-ls-remote" >/dev/null
grep -E '^create:.*--verify-tag' "$MOCK_STATE/calls" >/dev/null
if grep -E -- '--target[[:space:]]+'"$SHA" "$MOCK_STATE/calls" >/dev/null; then
    printf 'public release creation incorrectly targeted the private source SHA\n' >&2
    exit 1
fi

printf 'Release publication recovery tests passed.\n'
