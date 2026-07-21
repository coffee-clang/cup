#!/bin/sh

# Purpose: Verifies strict parsing of release-decision metadata before a
# candidate is accepted by release.yml.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin release-candidate
candidate=$TMP_ROOT/candidate
mkdir -p "$candidate"
VERSION=1.2.3
TAG=v$VERSION
SHA=0123456789abcdef0123456789abcdef01234567

run_candidate_info() {
    output=$1
    shift
    GITHUB_OUTPUT=$output "$@" \
        "$PROJECT_ROOT/scripts/release/candidate-info.sh" "$candidate"
}

write_decision() {
    cat > "$candidate/release-decision.env" <<EOF_DECISION
SHOULD_RELEASE=0
VERSION=$VERSION
TAG=$TAG
SHA=$SHA
EOF_DECISION
}

write_decision
run_candidate_info "$TMP_ROOT/output" >/dev/null
grep -Fx 'should_release=0' "$TMP_ROOT/output" >/dev/null
grep -Fx "version=$VERSION" "$TMP_ROOT/output" >/dev/null
grep -Fx "tag=$TAG" "$TMP_ROOT/output" >/dev/null
grep -Fx "sha=$SHA" "$TMP_ROOT/output" >/dev/null

write_decision
printf 'VERSION=%s\n' "$VERSION" >> "$candidate/release-decision.env"
if run_candidate_info "$TMP_ROOT/duplicate-output" >/dev/null 2>&1; then
    fail 'duplicate release-decision key unexpectedly passed validation'
fi

write_decision
sed '/^TAG=/d' "$candidate/release-decision.env" \
    > "$candidate/release-decision.env.next"
mv "$candidate/release-decision.env.next" "$candidate/release-decision.env"
if run_candidate_info "$TMP_ROOT/incomplete-output" >/dev/null 2>&1; then
    fail 'incomplete release-decision metadata unexpectedly passed validation'
fi


write_decision
if run_candidate_info "$TMP_ROOT/wrong-sha-output" \
        env EXPECTED_VERSION="$VERSION" EXPECTED_TAG="$TAG" \
        EXPECTED_SHA=1111111111111111111111111111111111111111 \
        >/dev/null 2>&1; then
    fail 'candidate from another source SHA unexpectedly passed validation'
fi

write_decision
run_candidate_info "$TMP_ROOT/expected-output" \
    env EXPECTED_VERSION="$VERSION" EXPECTED_TAG="$TAG" EXPECTED_SHA="$SHA" \
    >/dev/null

printf 'Release candidate metadata tests passed.\n'
