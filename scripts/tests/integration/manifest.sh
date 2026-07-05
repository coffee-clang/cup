#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin manifest
prepare_command_environment
manifest=$DEV_ROOT/config/packages.cfg
original=$TMP_ROOT/packages.cfg.original
cp "$manifest" "$original"

awk '
    !removed && /\.checksum_url_template=/ { removed = 1; next }
    { print }
    END { if (!removed) exit 2 }
' "$original" > "$manifest" || fail 'could not remove checksum_url_template'
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
' "$original" > "$manifest" || fail 'could not alter checksum URL'
run_cup_expect_failure "$TMP_ROOT/insecure-checksum.out" search
assert_contains "$(cat "$TMP_ROOT/insecure-checksum.out")" \
    'manifest URL templates must use HTTPS'

printf '%s\n' 'Manifest checksum schema tests passed.'
