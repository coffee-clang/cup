#!/usr/bin/env bash

# Exercises immutable, resumable and conflict-safe publication.
set -euo pipefail

TESTS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$TESTS_ROOT/support/common.sh"
test_begin release-publish
ROOT="$PROJECT_ROOT"

VERSION=1.2.3
TAG=v$VERSION
SHA=0123456789abcdef0123456789abcdef01234567
PUBLIC_SHA=89abcdef0123456789abcdef0123456789abcdef
TESTS_RUN_ID=31
TESTS_RUN_ATTEMPT=2
TESTS_INDEX_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
RELEASE_RUN_ID=41
RELEASE_RUN_ATTEMPT=3
DIST=$TMP_ROOT/dist
MOCK_BIN=$TMP_ROOT/bin
MOCK_STATE=$TMP_ROOT/state
REMOTE_ASSETS=$TMP_ROOT/remote-assets
mkdir -p "$DIST" "$MOCK_BIN" "$MOCK_STATE" "$REMOTE_ASSETS"

public_assets=(
    packages.cfg install.cfg release.txt provenance.txt THIRD_PARTY_NOTICES.txt
    install.sh install.ps1 uninstall.sh uninstall.ps1
    cup-linux-x64 cup-linux-arm64 cup-macos-x64 cup-macos-arm64
    cup-windows-x64.exe SHA256SUMS.common SHA256SUMS.linux-x64
    SHA256SUMS.linux-arm64 SHA256SUMS.macos-x64
    SHA256SUMS.macos-arm64 SHA256SUMS.windows-x64
)
printf '%s\n' "${public_assets[@]}" > "$MOCK_STATE/expected-assets"

write_candidate() {
    rm -rf "$DIST"
    mkdir -p "$DIST"
    printf 'packages\n' > "$DIST/packages.cfg"
    printf 'install configuration\n' > "$DIST/install.cfg"
    printf 'format=1\nversion=%s\ncommit=%s\n' "$VERSION" "$SHA" > "$DIST/release.txt"
    cat > "$DIST/provenance.txt" <<EOF_PROVENANCE
format=3
version=$VERSION
source_repository=example/cup-source
source_commit=$SHA
tests_run_id=$TESTS_RUN_ID
tests_run_attempt=$TESTS_RUN_ATTEMPT
tests_evidence_index_sha256=$TESTS_INDEX_SHA
release_run_id=$RELEASE_RUN_ID
release_run_attempt=$RELEASE_RUN_ATTEMPT
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
    write_checksums SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
    write_checksums SHA256SUMS.linux-x64 cup-linux-x64 uninstall.sh release.txt SHA256SUMS.common
    write_checksums SHA256SUMS.linux-arm64 cup-linux-arm64 uninstall.sh release.txt SHA256SUMS.common
    write_checksums SHA256SUMS.macos-x64 cup-macos-x64 uninstall.sh release.txt SHA256SUMS.common
    write_checksums SHA256SUMS.macos-arm64 cup-macos-arm64 uninstall.sh release.txt SHA256SUMS.common
    write_checksums SHA256SUMS.windows-x64 cup-windows-x64.exe uninstall.ps1 release.txt SHA256SUMS.common
    chmod 0755 "$DIST" "$DIST/install.sh" "$DIST/uninstall.sh" \
        "$DIST/cup-linux-x64" "$DIST/cup-linux-arm64" \
        "$DIST/cup-macos-x64" "$DIST/cup-macos-arm64"
    find "$DIST" -type f ! -perm -0100 -exec chmod 0644 {} +
}

write_checksums() {
    local output=$1
    shift
    : > "$DIST/$output"
    for name in "$@"; do
        printf '%s  %s\n' "$(hash_file "$DIST/$name")" "$name" >> "$DIST/$output"
    done
}
write_candidate

cat > "$MOCK_BIN/gh" <<'EOF_GH'
#!/usr/bin/env bash
set -euo pipefail

not_found() { printf 'HTTP 404: Not Found\n' >&2; exit 1; }
record_call() { printf '%s\n' "$1" >> "$MOCK_STATE/calls"; }
set_asset_name() {
    local name=$1
    grep -Fxq "$name" "$MOCK_STATE/assets" 2>/dev/null || printf '%s\n' "$name" >> "$MOCK_STATE/assets"
}
copy_argument_assets() {
    local argument name
    for argument in "$@"; do
        [ -f "$argument" ] || continue
        name=${argument##*/}
        cp "$argument" "$REMOTE_ASSETS/$name"
        set_asset_name "$name"
    done
}
copy_dist_assets() {
    rm -rf "$REMOTE_ASSETS"
    mkdir -p "$REMOTE_ASSETS"
    : > "$MOCK_STATE/assets"
    while IFS= read -r name; do
        cp "$MOCK_DIST/$name" "$REMOTE_ASSETS/$name"
        printf '%s\n' "$name" >> "$MOCK_STATE/assets"
    done < "$MOCK_STATE/expected-assets"
}

if [ "${MUTATE_CANDIDATE_ON_FIRST_API:-0}" = 1 ] && [ ! -f "$MOCK_STATE/mutated" ]; then
    : > "$MOCK_STATE/mutated"
    printf 'swapped-after-snapshot\n' > "$MOCK_DIST/packages.cfg"
fi

case "${1:-}" in
    api)
        endpoint=${2:-}
        case "$endpoint" in
            repos/*/commits/*)
                ref=${endpoint##*/}
                if [ "$ref" = "$TAG" ]; then
                    [ -f "$MOCK_STATE/tag-sha" ] || not_found
                    cat "$MOCK_STATE/tag-sha"
                elif [ "$ref" = "$SOURCE_SHA" ]; then
                    printf '%s\n' "$SOURCE_SHA"
                elif [ "$ref" = "$RELEASE_TARGET" ]; then
                    printf '%s\n' "$PUBLIC_SHA"
                else
                    exit 2
                fi
                ;;
            repos/*/releases/tags/*)
                count=0
                [ ! -f "$MOCK_STATE/release-query-count" ] || count=$(cat "$MOCK_STATE/release-query-count")
                count=$((count + 1))
                printf '%s\n' "$count" > "$MOCK_STATE/release-query-count"
                if [ "${CONCURRENT_ON_SECOND_RELEASE_QUERY:-0}" = 1 ] &&
                    [ "$count" -eq 2 ] && [ ! -f "$MOCK_STATE/release" ]; then
                    copy_dist_assets
                    : > "$MOCK_STATE/release"
                    printf 'true\n' > "$MOCK_STATE/draft"
                    printf '%s\n' "$PUBLIC_SHA" > "$MOCK_STATE/tag-sha"
                    record_call concurrent-draft
                fi
                [ -f "$MOCK_STATE/release" ] || not_found
                cat "$MOCK_STATE/draft"
                ;;
            *) exit 2 ;;
        esac
        ;;
    release)
        command=${2:-}
        shift 2
        case "$command" in
            view)
                [ -f "$MOCK_STATE/release" ] || exit 1
                cat "$MOCK_STATE/assets"
                ;;
            download)
                destination=
                pattern=
                while [ "$#" -gt 0 ]; do
                    case "$1" in
                        --dir) destination=$2; shift 2 ;;
                        --pattern) pattern=$2; shift 2 ;;
                        *) shift ;;
                    esac
                done
                [ -n "$destination" ] || exit 2
                mkdir -p "$destination"
                while IFS= read -r asset; do
                    [ -z "$pattern" ] || [ "$asset" = "$pattern" ] || continue
                    cp "$REMOTE_ASSETS/$asset" "$destination/$asset"
                done < "$MOCK_STATE/assets"
                ;;
            create)
                : > "$MOCK_STATE/release"
                printf 'true\n' > "$MOCK_STATE/draft"
                : > "$MOCK_STATE/assets"
                rm -rf "$REMOTE_ASSETS"; mkdir -p "$REMOTE_ASSETS"
                copy_argument_assets "$@"
                target=
                previous=
                for argument in "$@"; do
                    [ "$previous" != --target ] || target=$argument
                    previous=$argument
                done
                if [ -n "$target" ]; then
                    printf '%s\n' "$target" > "$MOCK_STATE/tag-sha"
                else
                    [ -f "$MOCK_STATE/tag-sha" ] || exit 2
                fi
                record_call create
                ;;
            delete-asset)
                asset=${2:-}
                grep -Fvx "$asset" "$MOCK_STATE/assets" > "$MOCK_STATE/assets.next" || true
                mv "$MOCK_STATE/assets.next" "$MOCK_STATE/assets"
                rm -f -- "$REMOTE_ASSETS/$asset"
                record_call "delete:$asset"
                ;;
            upload)
                copy_argument_assets "$@"
                record_call upload
                ;;
            edit)
                printf 'false\n' > "$MOCK_STATE/draft"
                record_call edit
                ;;
            *) exit 2 ;;
        esac
        ;;
    *) exit 2 ;;
esac
EOF_GH
chmod +x "$MOCK_BIN/gh"

run_publish() {
    PATH="$MOCK_BIN:$PATH" MOCK_STATE="$MOCK_STATE" MOCK_DIST="$DIST" \
        REMOTE_ASSETS="$REMOTE_ASSETS" SOURCE_SHA="$SHA" PUBLIC_SHA="$PUBLIC_SHA" \
        RELEASE_TARGET=public-main TAG="$TAG" VERSION="$VERSION" SHA="$SHA" \
        SOURCE_REPOSITORY=example/cup-source TESTS_RUN_ID="$TESTS_RUN_ID" \
        TESTS_RUN_ATTEMPT="$TESTS_RUN_ATTEMPT" \
        TESTS_EVIDENCE_INDEX_SHA256="$TESTS_INDEX_SHA" \
        RELEASE_RUN_ID="$RELEASE_RUN_ID" RELEASE_RUN_ATTEMPT="$RELEASE_RUN_ATTEMPT" \
        GH_TOKEN=test GH_REPO=example/cup-public \
        MUTATE_CANDIDATE_ON_FIRST_API="${MUTATE_CANDIDATE_ON_FIRST_API:-0}" \
        CONCURRENT_ON_SECOND_RELEASE_QUERY="${CONCURRENT_ON_SECOND_RELEASE_QUERY:-0}" \
        "$ROOT/scripts/release/publish.sh" "$DIST"
}

reset_remote() {
    rm -f -- "$MOCK_STATE/release" "$MOCK_STATE/draft" "$MOCK_STATE/assets" \
        "$MOCK_STATE/tag-sha" "$MOCK_STATE/calls" "$MOCK_STATE/release-query-count" \
        "$MOCK_STATE/mutated"
    rm -rf -- "$REMOTE_ASSETS"
    mkdir -p "$REMOTE_ASSETS"
}

# Candidate assembly restores deterministic POSIX modes after artifact transport.
part_common=$TMP_ROOT/part-common
part_platform=$TMP_ROOT/part-platform
assembled=$TMP_ROOT/assembled
mkdir -p "$part_common" "$part_platform"
for asset in "${public_assets[@]}"; do
    case "$asset" in
        cup-*) cp "$DIST/$asset" "$part_platform/$asset" ;;
        *) cp "$DIST/$asset" "$part_common/$asset" ;;
    esac
    chmod 0644 "$part_common/$asset" 2>/dev/null || true
    chmod 0644 "$part_platform/$asset" 2>/dev/null || true
done
"$ROOT/scripts/release/assemble-candidate.sh" "$assembled"     "$part_common" "$part_platform" >/dev/null
for executable in install.sh uninstall.sh cup-linux-x64 cup-linux-arm64         cup-macos-x64 cup-macos-arm64; do
    [ "$(stat -c '%a' "$assembled/$executable")" = 755 ] ||
        fail "assembled executable mode is not 0755: $executable"
done
[ "$(stat -c '%a' "$assembled/install.ps1")" = 644 ] ||
    fail 'assembled non-POSIX asset mode is not 0644'
[ "$(stat -c '%a' "$assembled")" = 755 ] ||
    fail 'assembled candidate directory mode is not 0755'

# Invalid provenance fails before any GitHub operation.
cp "$DIST/provenance.txt" "$TMP_ROOT/provenance.valid"
printf 'release_run_attempt=%s\n' "$RELEASE_RUN_ATTEMPT" >> "$DIST/provenance.txt"
if run_publish > "$TMP_ROOT/provenance.out" 2>&1; then
    fail 'duplicate provenance unexpectedly passed validation'
fi
assert_contains "$(cat "$TMP_ROOT/provenance.out")" 'invalid provenance file'
mv "$TMP_ROOT/provenance.valid" "$DIST/provenance.txt"

# Fresh publication and an already-published exact no-op.
reset_remote
run_publish >/dev/null
assert_equals "$(cat "$MOCK_STATE/tag-sha")" "$PUBLIC_SHA"
assert_equals "$(cat "$MOCK_STATE/draft")" false
[ "$(grep -c '^create$' "$MOCK_STATE/calls")" -eq 1 ]
[ "$(grep -c '^edit$' "$MOCK_STATE/calls")" -eq 1 ]
mutations_before=$(grep -Ec '^(create|upload|delete:|edit)' "$MOCK_STATE/calls")
run_publish >/dev/null
mutations_after=$(grep -Ec '^(create|upload|delete:|edit)' "$MOCK_STATE/calls")
[ "$mutations_before" -eq "$mutations_after" ] || fail 'published release was mutated'

# Byte equality, not only names, is required.
printf 'corrupt\n' >> "$REMOTE_ASSETS/packages.cfg"
if run_publish > "$TMP_ROOT/corrupt.out" 2>&1; then
    fail 'corrupt published asset unexpectedly passed verification'
fi
assert_contains "$(cat "$TMP_ROOT/corrupt.out")" 'immutable candidate: packages.cfg'

# An ambiguous draft is preserved unchanged.
reset_remote
: > "$MOCK_STATE/release"
printf 'true\n' > "$MOCK_STATE/draft"
printf '%s\n' "$PUBLIC_SHA" > "$MOCK_STATE/tag-sha"
printf 'packages.cfg\nunexpected.bin\n' > "$MOCK_STATE/assets"
printf 'stale\n' > "$REMOTE_ASSETS/packages.cfg"
printf 'unexpected\n' > "$REMOTE_ASSETS/unexpected.bin"
if run_publish > "$TMP_ROOT/ambiguous.out" 2>&1; then
    fail 'ambiguous draft unexpectedly resumed'
fi
assert_contains "$(cat "$TMP_ROOT/ambiguous.out")" 'preserving it unchanged'
[ ! -f "$MOCK_STATE/calls" ] || ! grep -Eq '^(delete:|upload|edit)' "$MOCK_STATE/calls"
[ -f "$REMOTE_ASSETS/unexpected.bin" ] || fail 'ambiguous draft asset was removed'

# A recognizable partial draft is reconciled and published.
reset_remote
: > "$MOCK_STATE/release"
printf 'true\n' > "$MOCK_STATE/draft"
printf '%s\n' "$PUBLIC_SHA" > "$MOCK_STATE/tag-sha"
printf 'provenance.txt\nunexpected.bin\n' > "$MOCK_STATE/assets"
cp "$DIST/provenance.txt" "$REMOTE_ASSETS/provenance.txt"
printf 'unexpected\n' > "$REMOTE_ASSETS/unexpected.bin"
run_publish >/dev/null
grep -Fxq 'delete:unexpected.bin' "$MOCK_STATE/calls"
grep -Fxq upload "$MOCK_STATE/calls"
grep -Fxq edit "$MOCK_STATE/calls"

# Candidate mutation after snapshot creation cannot affect uploaded bytes.
reset_remote
write_candidate
cp "$DIST/packages.cfg" "$TMP_ROOT/packages.snapshot"
MUTATE_CANDIDATE_ON_FIRST_API=1 run_publish >/dev/null
cmp -s "$REMOTE_ASSETS/packages.cfg" "$TMP_ROOT/packages.snapshot" ||
    fail 'publisher uploaded bytes reopened from the mutable candidate path'
write_candidate

# A release appearing between the two absence checks is recognized and resumed.
reset_remote
CONCURRENT_ON_SECOND_RELEASE_QUERY=1 run_publish >/dev/null
grep -Fxq concurrent-draft "$MOCK_STATE/calls"
[ ! -e "$MOCK_STATE/calls.tmp" ]
assert_equals "$(cat "$MOCK_STATE/draft")" false

# A conflicting existing tag is never reused.
reset_remote
printf '%s\n' 1111111111111111111111111111111111111111 > "$MOCK_STATE/tag-sha"
if run_publish > "$TMP_ROOT/tag-conflict.out" 2>&1; then
    fail 'conflicting tag unexpectedly passed publication'
fi
assert_contains "$(cat "$TMP_ROOT/tag-conflict.out")" "tag $TAG points to"
[ ! -e "$MOCK_STATE/release" ]

printf 'Release publication recovery tests passed.\n'
