#!/usr/bin/env sh

# Purpose: Validates a downloaded build-run artifact set and exports canonical candidate metadata.
# The no-op and release-candidate layouts are checked separately.
set -eu

. "$(dirname "$0")/common.sh"

dist=${1:-candidate}
[ -n "${GITHUB_OUTPUT:-}" ] || fail 'GITHUB_OUTPUT is not set'

# Parse the no-op/build decision as an exact schema; duplicates are rejected.
read_release_decision() {
    file=$1
    seen_should_release=0
    seen_version=0
    seen_tag=0
    seen_sha=0

    while IFS='=' read -r key value; do
        case "$key" in
            SHOULD_RELEASE)
                [ "$seen_should_release" -eq 0 ] ||
                    fail "duplicate candidate metadata key in $file: $key"
                SHOULD_RELEASE=$value
                seen_should_release=1
                ;;
            VERSION)
                [ "$seen_version" -eq 0 ] ||
                    fail "duplicate candidate metadata key in $file: $key"
                VERSION=$value
                seen_version=1
                ;;
            TAG)
                [ "$seen_tag" -eq 0 ] ||
                    fail "duplicate candidate metadata key in $file: $key"
                TAG=$value
                seen_tag=1
                ;;
            SHA)
                [ "$seen_sha" -eq 0 ] ||
                    fail "duplicate candidate metadata key in $file: $key"
                SHA=$value
                seen_sha=1
                ;;
            *)
                fail "unexpected candidate metadata key in $file: $key"
                ;;
        esac
    done < "$file"

    [ "$seen_should_release" -eq 1 ] && [ "$seen_version" -eq 1 ] &&
        [ "$seen_tag" -eq 1 ] && [ "$seen_sha" -eq 1 ] ||
        fail "incomplete release decision metadata: $file"
}

require_asset() {
    [ -f "$dist/$1" ] || fail "missing candidate asset: $1"
}

SHOULD_RELEASE=
VERSION=
TAG=
SHA=

# Distinguish a no-op decision from a complete release candidate.
require_asset release-decision.env
read_release_decision "$dist/release-decision.env"

case "$SHOULD_RELEASE" in
    0|1) ;;
    *)
        fail "invalid SHOULD_RELEASE value: $SHOULD_RELEASE"
        ;;
esac

validate_release_inputs

[ -z "${EXPECTED_VERSION:-}" ] || [ "$VERSION" = "$EXPECTED_VERSION" ] ||
    fail 'candidate VERSION does not match the selected source revision'
[ -z "${EXPECTED_TAG:-}" ] || [ "$TAG" = "$EXPECTED_TAG" ] ||
    fail 'candidate TAG does not match the selected source revision'
[ -z "${EXPECTED_SHA:-}" ] || [ "$SHA" = "$EXPECTED_SHA" ] ||
    fail 'candidate SHA does not match the selected source revision'

if [ "$SHOULD_RELEASE" = 0 ]; then
    {
        printf 'should_release=0\n'
        printf 'version=%s\n' "$VERSION"
        printf 'tag=%s\n' "$TAG"
        printf 'sha=%s\n' "$SHA"
    } >> "$GITHUB_OUTPUT"
    info "No release is needed for $TAG; the tag already exists."
    exit 0
fi

# Complete candidates must agree across decision, identity, provenance and assets.
require_asset candidate.env
candidate_version=
candidate_tag=
candidate_sha=
seen_candidate_version=0
seen_candidate_tag=0
seen_candidate_sha=0
while IFS='=' read -r key value; do
    case "$key" in
        VERSION)
            [ "$seen_candidate_version" -eq 0 ] ||
                fail "duplicate candidate metadata key in $dist/candidate.env: $key"
            candidate_version=$value
            seen_candidate_version=1
            ;;
        TAG)
            [ "$seen_candidate_tag" -eq 0 ] ||
                fail "duplicate candidate metadata key in $dist/candidate.env: $key"
            candidate_tag=$value
            seen_candidate_tag=1
            ;;
        SHA)
            [ "$seen_candidate_sha" -eq 0 ] ||
                fail "duplicate candidate metadata key in $dist/candidate.env: $key"
            candidate_sha=$value
            seen_candidate_sha=1
            ;;
        *)
            fail "unexpected candidate metadata key in $dist/candidate.env: $key"
            ;;
    esac
done < "$dist/candidate.env"
[ "$seen_candidate_version" -eq 1 ] && [ "$seen_candidate_tag" -eq 1 ] &&
    [ "$seen_candidate_sha" -eq 1 ] ||
    fail "incomplete candidate metadata: $dist/candidate.env"
[ "$candidate_version" = "$VERSION" ] || fail 'candidate VERSION does not match release decision'
[ "$candidate_tag" = "$TAG" ] || fail 'candidate TAG does not match release decision'
[ "$candidate_sha" = "$SHA" ] || fail 'candidate SHA does not match release decision'

for asset in \
    packages.cfg install.cfg release.txt provenance.txt THIRD_PARTY_NOTICES.txt \
    install.sh install.ps1 uninstall.sh uninstall.ps1 \
    cup-linux-x64 cup-linux-arm64 cup-macos-x64 cup-macos-arm64 \
    cup-windows-x64.exe SHA256SUMS.common \
    SHA256SUMS.linux-x64 SHA256SUMS.linux-arm64 \
    SHA256SUMS.macos-x64 SHA256SUMS.macos-arm64 \
    SHA256SUMS.windows-x64; do
    require_asset "$asset"
done

test "$(sed -n 's/^format=//p' "$dist/release.txt")" = 1
test "$(sed -n 's/^version=//p' "$dist/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$dist/release.txt")" = "$SHA"
test "$(wc -l < "$dist/release.txt" | tr -d '[:space:]')" = 3

# Bind the candidate to the exact successful Tests run selected by release.yml.
validate_provenance_file \
    "$dist/provenance.txt" \
    "${EXPECTED_SOURCE_REPOSITORY:-}" \
    "${EXPECTED_TESTS_RUN_ID:-}" \
    "${EXPECTED_TESTS_RUN_ATTEMPT:-}"

# Verify every platform checksum after the complete asset set is present.
verify_checksum_file_exact "$dist" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$dist" SHA256SUMS.linux-x64 cup-linux-x64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.linux-arm64 cup-linux-arm64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.macos-x64 cup-macos-x64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.macos-arm64 cup-macos-arm64 uninstall.sh release.txt
verify_checksum_file_exact "$dist" SHA256SUMS.windows-x64 \
    cup-windows-x64.exe uninstall.ps1 release.txt

{
    printf 'should_release=1\n'
    printf 'version=%s\n' "$VERSION"
    printf 'tag=%s\n' "$TAG"
    printf 'sha=%s\n' "$SHA"
    printf 'source_repository=%s\n' \
        "$(provenance_value source_repository "$dist/provenance.txt")"
    printf 'tests_run_id=%s\n' \
        "$(provenance_value tests_run_id "$dist/provenance.txt")"
    printf 'tests_run_attempt=%s\n' \
        "$(provenance_value tests_run_attempt "$dist/provenance.txt")"
} >> "$GITHUB_OUTPUT"

info "Testing release candidate $TAG at $SHA."
