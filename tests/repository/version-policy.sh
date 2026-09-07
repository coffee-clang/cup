#!/bin/sh

# Verifies VERSION, Git and release-mode rules for official and development identifiers.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin version
repo=$TMP_ROOT/repository
mkdir -p "$repo/scripts/lib"
cp "$PROJECT_ROOT/scripts/version.sh" "$repo/scripts/version.sh"
cp "$PROJECT_ROOT/scripts/lib/path-safety.sh" \
    "$PROJECT_ROOT/scripts/lib/build-configuration.sh" \
    "$PROJECT_ROOT/scripts/lib/git-identity.sh" \
    "$PROJECT_ROOT/scripts/lib/semver.sh" "$repo/scripts/lib/"
printf '%s\n' '0.2.0' > "$repo/VERSION"
chmod +x "$repo/scripts/version.sh"

printf '%s\n' '00.2.0' > "$repo/VERSION"
if (cd "$repo" && ./scripts/version.sh base >/dev/null 2>&1); then
    fail 'VERSION with a leading zero unexpectedly succeeded'
fi
printf '0.2.0\r\n' > "$repo/VERSION"
if (cd "$repo" && ./scripts/version.sh base >/dev/null 2>&1); then
    fail 'VERSION with CRLF unexpectedly succeeded'
fi
printf '0.2.0\0\n' > "$repo/VERSION"
if (cd "$repo" && ./scripts/version.sh base >/dev/null 2>&1); then
    fail 'VERSION with a hidden NUL byte unexpectedly succeeded'
fi
printf '0.2.0' > "$repo/VERSION"
if (cd "$repo" && ./scripts/version.sh base >/dev/null 2>&1); then
    fail 'VERSION without a final LF unexpectedly succeeded'
fi
printf '%s\n' '999999.999999.999999' > "$repo/VERSION"
assert_equals "$(cd "$repo" && ./scripts/version.sh base)" '999999.999999.999999'
printf '%s\n' '1000000.0.0' > "$repo/VERSION"
if (cd "$repo" && ./scripts/version.sh base >/dev/null 2>&1); then
    fail 'VERSION component above 999999 unexpectedly succeeded'
fi
printf '%s\n' '0.2.0' > "$repo/VERSION"

(
    cd "$repo"
    assert_equals "$(./scripts/version.sh base)" '0.2.0'
    assert_equals "$(./scripts/version.sh current)" '0.2.0-dev+archive'
    ./scripts/version.sh generate generated-archive
    assert_equals "$(sed -n 's/^commit=//p' generated-archive/release.txt)" \
        '0000000000000000000000000000000000000000'
    assert_equals "$(wc -l < generated-archive/release.txt | tr -d '[:space:]')" '3'
    grep -Fx '#define CUP_VERSION_COMMIT "archive"' generated-archive/version.h >/dev/null ||
        fail 'archive development header lost its human-readable identity'
    rm -rf generated-archive

    if CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
        ./scripts/version.sh generate rejected-official-archive >/dev/null 2>&1; then
        fail 'official source-archive metadata unexpectedly succeeded'
    fi
    [ ! -e rejected-official-archive ] || fail 'rejected official archive left output behind'

    git -c init.defaultBranch=main init -q
    git config user.email cup-tests@example.invalid
    git config user.name 'cup tests'
    git add VERSION scripts/version.sh scripts/lib/path-safety.sh \
        scripts/lib/build-configuration.sh scripts/lib/git-identity.sh scripts/lib/semver.sh
    git commit -qm initial

    assert_contains "$(./scripts/version.sh current)" '0.2.0-dev.1+'
    git tag v0.2.0
    assert_contains "$(./scripts/version.sh current)" '0.2.0-dev.0+'
    if CUP_OFFICIAL_BUILD=1 ./scripts/version.sh current >/dev/null 2>&1; then
        fail 'official identity without release configuration unexpectedly succeeded'
    fi
    current=$(CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh current)
    validated=$(CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
        ./scripts/version.sh validate-release)
    assert_equals "$current" '0.2.0'
    assert_equals "$validated" '0.2.0'
    CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh generate generated
    assert_equals "$(sed -n 's/^format=//p' generated/release.txt)" '1'
    assert_equals "$(sed -n 's/^version=//p' generated/release.txt)" '0.2.0'
    assert_equals "$(sed -n 's/^commit=//p' generated/release.txt)" \
        "$(git rev-parse HEAD)"
    assert_equals "$(wc -l < generated/release.txt | tr -d '[:space:]')" '3'
    grep -Fx '#include "version.h"' generated/version.rc >/dev/null ||
        fail 'Windows version resource does not include generated version metadata'
    grep -F '<longPathAware xmlns=""http://schemas.microsoft.com/SMI/2016/WindowsSettings"">true</longPathAware>' \
        generated/version.rc >/dev/null ||
        fail 'Windows version resource is not long-path aware'
    grep -F 'version=""1.0""' generated/version.rc >/dev/null ||
        fail 'Windows version resource does not use RC quote escaping'
    if grep -F '\"' generated/version.rc >/dev/null; then
        fail 'Windows version resource uses C escaping inside RC strings'
    fi
    grep -Fx '#define CUP_VERSION_OFFICIAL 1' generated/version.h >/dev/null ||
        fail 'official generated header does not mark the build as official'

    printf '%s\n' change > change.txt
    git add change.txt
    git commit -qm development
    assert_contains "$(./scripts/version.sh current)" '0.2.0-dev.1+'

    printf '%s\n' '0.3.0' > VERSION
    git add VERSION
    git commit -qm 'prepare next version'
    assert_contains "$(./scripts/version.sh current)" '0.3.0-dev.2+'

    printf '%s\n' dirty >> change.txt
    assert_contains "$(./scripts/version.sh current)" '.dirty'
    if CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh validate-release >/dev/null 2>&1; then
        fail 'dirty release validation unexpectedly succeeded'
    fi

    printf '%s\n' '0.2.1' > VERSION
    if CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release ./scripts/version.sh validate-release >/dev/null 2>&1; then
        fail 'mismatched VERSION/tag unexpectedly succeeded'
    fi
)

printf '%s\n' 'Version policy tests passed.'
