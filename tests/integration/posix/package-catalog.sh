#!/bin/sh

# Purpose: Exercises installed and development catalog selection, plus
# invalid-catalog behavior through the real CLI.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin catalog
prepare_command_environment
catalog=$DEV_ROOT/config/packages.cfg
original=$TMP_ROOT/packages.cfg.original
cp "$catalog" "$original"

awk '
    !removed && /\.checksum_url_template=/ { removed = 1; next }
    { print }
    END { if (!removed) exit 2 }
' "$original" > "$catalog" || fail 'could not remove checksum_url_template'
run_cup_expect_failure "$TMP_ROOT/missing-checksum.out" search
assert_contains "$(cat "$TMP_ROOT/missing-checksum.out")" \
    'is missing one or more required fields'

awk '
    !changed && /\.checksum_url_template=https:/ {
        sub(/=https:/, "=http:")
        changed = 1
    }
    { print }
    END { if (!changed) exit 2 }
' "$original" > "$catalog" || fail 'could not alter checksum URL'
run_cup_expect_failure "$TMP_ROOT/insecure-checksum.out" search
assert_contains "$(cat "$TMP_ROOT/insecure-checksum.out")" \
    'catalog URL templates must use HTTPS'

printf '%s\n' 'PackageCatalog checksum schema tests passed.'
