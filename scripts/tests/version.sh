#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

test_begin version
repo=$TMP_ROOT/repository
mkdir -p "$repo"
cp "$PROJECT_ROOT/scripts/version.sh" "$repo/version.sh"
chmod +x "$repo/version.sh"

(
    cd "$repo"
    git init -q
    git config user.email cup-tests@example.invalid
    git config user.name 'cup tests'
    git add version.sh
    git commit -qm initial

    output=$(./version.sh plan auto)
    assert_contains "$output" 'publish=1'
    assert_contains "$output" 'version=0.1.0'

    git tag v0.1.0
    assert_equals "$(./version.sh current)" '0.1.0'

    i=1
    while [ "$i" -le 9 ]; do
        printf '%s\n' "$i" >> history.txt
        git add history.txt
        git commit -qm "development $i"
        i=$((i + 1))
    done

    assert_contains "$(./version.sh current)" '0.1.0-dev.9+g'
    assert_contains "$(./version.sh plan auto)" 'publish=0'

    mkdir -p certs
    printf 'test certificate update\n' > certs/cacert.pem
    git add certs/cacert.pem
    git commit -qm 'Update certs'
    assert_equals "$(./version.sh distance)" '9'
    assert_contains "$(./version.sh current)" '0.1.0-dev.9+g'
    assert_contains "$(./version.sh plan auto)" 'publish=0'

    printf '10\n' >> history.txt
    git add history.txt
    git commit -qm 'development 10'
    output=$(./version.sh plan auto)
    assert_contains "$output" 'publish=1'
    assert_contains "$output" 'version=0.1.1'

    git tag v0.1.9
    i=1
    while [ "$i" -le 10 ]; do
        printf 'patch-%s\n' "$i" >> history.txt
        git add history.txt
        git commit -qm "patch development $i"
        i=$((i + 1))
    done

    assert_contains "$(./version.sh plan auto)" 'version=0.1.10'
    assert_contains "$(./version.sh plan minor)" 'version=0.2.0'
    assert_contains "$(./version.sh plan major)" 'version=1.0.0'

    printf 'dirty\n' >> history.txt
    assert_contains "$(./version.sh current)" '.dirty'
)

printf '%s\n' 'Version policy tests passed.'
