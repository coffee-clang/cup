#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin version
repo=$TMP_ROOT/repository
mkdir -p "$repo"
cp "$PROJECT_ROOT/scripts/version.sh" "$repo/version.sh"
printf '%s\n' '0.2.0' > "$repo/VERSION"
chmod +x "$repo/version.sh"

printf '%s\n' '00.2.0' > "$repo/VERSION"
if (cd "$repo" && ./version.sh base >/dev/null 2>&1); then
    fail 'VERSION with a leading zero unexpectedly succeeded'
fi
printf '%s\n' '0.2.0' > "$repo/VERSION"

(
    cd "$repo"
    assert_equals "$(./version.sh base)" '0.2.0'
    assert_equals "$(./version.sh current)" '0.2.0-dev+archive'

    git init -q
    git config user.email cup-tests@example.invalid
    git config user.name 'cup tests'
    git add VERSION version.sh
    git commit -qm initial

    assert_contains "$(./version.sh current)" '0.2.0-dev.1+'
    git tag v0.2.0
    assert_contains "$(./version.sh current)" '0.2.0-dev.0+'
    assert_equals "$(CUP_RELEASE_BUILD=1 ./version.sh current)" '0.2.0'
    assert_equals "$(CUP_RELEASE_BUILD=1 ./version.sh validate-release)" '0.2.0'
    CUP_RELEASE_BUILD=1 ./version.sh generate generated
    assert_equals "$(sed -n 's/^format=//p' generated/release.txt)" '1'
    assert_equals "$(sed -n 's/^version=//p' generated/release.txt)" '0.2.0'
    assert_equals "$(sed -n 's/^commit=//p' generated/release.txt)" \
        "$(git rev-parse HEAD)"
    assert_equals "$(wc -l < generated/release.txt | tr -d '[:space:]')" '3'

    printf '%s\n' change > change.txt
    git add change.txt
    git commit -qm development
    assert_contains "$(./version.sh current)" '0.2.0-dev.1+'

    printf '%s\n' '0.3.0' > VERSION
    git add VERSION
    git commit -qm 'prepare next version'
    assert_contains "$(./version.sh current)" '0.3.0-dev.2+'

    printf '%s\n' dirty >> change.txt
    assert_contains "$(./version.sh current)" '.dirty'
    if CUP_RELEASE_BUILD=1 ./version.sh validate-release >/dev/null 2>&1; then
        fail 'dirty release validation unexpectedly succeeded'
    fi

    printf '%s\n' '0.2.1' > VERSION
    if CUP_RELEASE_BUILD=1 ./version.sh validate-release >/dev/null 2>&1; then
        fail 'mismatched VERSION/tag unexpectedly succeeded'
    fi
)

printf '%s\n' 'Version policy tests passed.'
