#!/bin/sh

# Purpose: Verifies VERSION, Git and release-mode rules for official and development identifiers.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

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
    if CUP_OFFICIAL_BUILD=1 ./version.sh current >/dev/null 2>&1; then
        fail 'official identity without release configuration unexpectedly succeeded'
    fi
    assert_equals "$(CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./version.sh current)" '0.2.0'
    assert_equals "$(CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./version.sh validate-release)" '0.2.0'
    CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./version.sh generate generated
    assert_equals "$(sed -n 's/^format=//p' generated/release.txt)" '1'
    assert_equals "$(sed -n 's/^version=//p' generated/release.txt)" '0.2.0'
    assert_equals "$(sed -n 's/^commit=//p' generated/release.txt)" \
        "$(git rev-parse HEAD)"
    assert_equals "$(wc -l < generated/release.txt | tr -d '[:space:]')" '3'
    grep -Fx '#include "version.h"' generated/version.rc >/dev/null ||
        fail 'Windows version resource does not include generated version metadata'
    grep -F '<longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>' \
        generated/version.rc >/dev/null ||
        fail 'Windows version resource is not long-path aware'
    grep -Fx '#define CUP_VERSION_OFFICIAL 1' generated/version.h >/dev/null ||
        fail 'official generated header does not mark the build as official'

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
    if CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./version.sh validate-release >/dev/null 2>&1; then
        fail 'dirty release validation unexpectedly succeeded'
    fi

    printf '%s\n' '0.2.1' > VERSION
    if CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./version.sh validate-release >/dev/null 2>&1; then
        fail 'mismatched VERSION/tag unexpectedly succeeded'
    fi
)

printf '%s\n' 'Version policy tests passed.'
